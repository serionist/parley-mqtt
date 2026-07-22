"""CR1/HC1 — cancelled-task reconciliation in ``_on_task_done`` (handler.py).

A wire ``cnl`` on an EXECUTING command both aborts the per-command signal and calls ``task.cancel()``.
Three shapes of cancellation must all reconcile to the correct terminal:

(a) ``cnl`` delivered in the SAME event-loop batch as ``req`` — the dispatched task has not taken its
    first step, so ``CancelledError`` lands at the coroutine entry, bypassing ``_run_command``'s
    try/except. ``_on_task_done`` must drive ``$cancelled`` (signal aborted), free the execution slot,
    stop the heartbeat cadence, and un-wedge the serial handler.
(b) an EXTERNAL ``task.cancel()`` whose signal is NOT aborted — the wrapper re-raises, and
    ``_on_task_done`` must abort the command (no result, slot freed, id tombstoned).
(c) a ``cnl`` that arrives AFTER the task started — the wrapper itself drives completion; the
    done-callback must NOT double-complete.
"""

from __future__ import annotations

from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock

_CANCELLED_RES = {"v": 1, "t": "res", "id": "A", "ok": False, "e": "$cancelled"}


async def test_same_batch_req_then_cnl_reconciles_and_unwedges() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)  # serial: max_concurrent_executions=1
    transport.clear_outbound()

    # req then cnl in the SAME batch — NO drain between. The dispatched task never takes its first
    # step before _on_cancel's task.cancel(), so CancelledError lands at the coroutine entry and
    # _run_command's discrimination is bypassed entirely.
    transport.deliver("request", wire.req("A"))
    transport.deliver("request", wire.cnl("A"))
    await drain()

    # _on_task_done reconciled from state (signal aborted) → $cancelled via normal completion.
    assert transport.publishes("result") == [_CANCELLED_RES]
    assert handler._executing == 0  # execution slot freed (pre-fix this stays wedged at 1)
    assert handler._command_count == 1  # A sits in RESULT-CACHED awaiting a rak
    assert not handler.is_executing("A")  # its task never ran the handler body
    assert handler._task_count == 0

    # No heartbeat storm: advance well past several heartbeat intervals; A's cadence is stopped, so no
    # hb for A is published (only its result-retry re-publishes the cached $cancelled on RESULT).
    transport.clear_outbound()
    clock.advance_by_ms(20_000)
    await drain()
    assert transport.publish_count("heartbeat", "hb") == 0

    # rak A to release its result-retry timers, then prove the serial handler is un-wedged: a NEW
    # command actually dispatches (pre-fix the single execution slot never freed, so B would wait).
    transport.deliver("request", wire.rak("A"))
    await drain()
    assert handler._command_count == 0
    assert handler._executing == 0

    transport.clear_outbound()
    transport.deliver("request", wire.req("B"))
    await drain()
    assert handler.is_executing("B")  # a task actually started — the slot was free
    assert handler._executing == 1
    assert transport.publish_count("heartbeat", "hb") >= 1  # B's accept-ack heartbeat

    # Drive B to a clean terminal so nothing is left parked; counters return to baseline.
    handler.complete("B", {"r": 2})
    await drain()
    transport.deliver("request", wire.rak("B"))
    await drain()
    assert handler._command_count == 0
    assert handler._executing == 0
    assert handler._queue_count == 0
    assert handler._task_count == 0
    assert handler._tombstone_count == 2  # A and B tombstoned
    assert handler._live_timer_count == 2  # only the two tombstone TTL timers remain
    assert clock.pending_count == 2

    await handler.close()


async def test_external_cancel_of_started_task_aborts() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    task = next(iter(handler._tasks))  # started + parked on its future
    assert handler.is_executing("A")
    transport.clear_outbound()

    # Cancel the execution task WITHOUT aborting the signal — cancellation the library did not cause.
    # The wrapper re-raises (asyncio semantics); _on_task_done must take the abort path.
    task.cancel()
    await drain()

    assert task.cancelled()
    assert transport.publish_count("result", "res") == 0  # no res produced
    assert handler._executing == 0  # slot freed
    assert handler._command_count == 0  # command removed (not left EXECUTING)
    assert handler._tombstone_count == 1  # id tombstoned
    assert handler._queue_count == 0
    assert handler._task_count == 0
    assert handler._live_timer_count == 1  # only the tombstone TTL timer remains

    await handler.close()


async def test_cancel_after_task_started_completes_exactly_once() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    task = next(iter(handler._tasks))  # started + parked, so the cnl injects into a live await
    assert handler.is_executing("A")
    transport.clear_outbound()

    # cnl AFTER the task started: the wrapper catches CancelledError while aborted and drives the
    # $cancelled completion itself. The done-callback must NOT drive a second one.
    transport.deliver("request", wire.cnl("A"))
    await drain()

    assert transport.publishes("result") == [_CANCELLED_RES]  # exactly one res
    assert not task.cancelled()  # wrapper caught + returned normally; the task is not cancelled
    assert task.done()
    assert handler._executing == 0  # decremented exactly once (a double would underflow to -1)
    assert handler._command_count == 1  # RESULT-CACHED awaiting rak

    # A further drain must not spawn a second completion from the done-callback.
    transport.clear_outbound()
    await drain()
    assert transport.publishes("result") == []

    transport.deliver("request", wire.rak("A"))
    await drain()
    assert handler._command_count == 0
    assert handler._executing == 0
    assert handler._tombstone_count == 1

    await handler.close()


async def test_external_cancel_reconcile_ignored_after_close() -> None:
    # A cancelled task whose done-callback fires after close() must not touch state: close() already
    # cancelled + is awaiting the tasks and cleared the table. _reconcile_cancelled_task no-ops when
    # disposed, so no spurious $cancelled and no counter underflow.
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    assert handler.is_executing("A")
    transport.clear_outbound()

    await handler.close()  # cancels A's task, awaits it, clears all state

    assert transport.publish_count("result", "res") == 0  # no $cancelled leaked on the way out
    assert handler._command_count == 0
    assert handler._executing == 0
    assert handler._tombstone_count == 0
    assert handler._task_count == 0
