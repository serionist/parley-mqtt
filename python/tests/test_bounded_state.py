"""Bounded state — sustained mixed load returns to steady state, both roles (testing.md layer 1).

Thousands of mixed cycles (success / custom failure / $busy / timeouts / cancel), then every internal
collection, every live timer handle, every registered clock timer, and every pending asyncio task is
asserted back at baseline. Zero event-loop warnings are enforced by the ``loop_error_guard`` fixture.
"""

from __future__ import annotations

import asyncio

from parley import ParleyCancelSignal, ParleyCommander, ParleyConfig
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS
from support.scripted_id_generator import ScriptedIdGenerator
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock

CYCLES = 3000


def _pending_tasks() -> list[asyncio.Task[object]]:
    current = asyncio.current_task()
    return [t for t in asyncio.all_tasks() if t is not current]


async def test_commander_sustained_load_returns_to_steady_state() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport,
        TOPICS,
        ParleyConfig(
            request_retry_interval=1.0,
            no_ack_timeout=2.5,
            stale_timeout=2.5,
            execution_timeout=10.0,
            cancel_retry_interval=1.0,
            tombstone_ttl=3.0,
        ),
        id_generator=gen,
        clock=clock,
    )
    transport.clear_outbound()

    for i in range(CYCLES):
        cid = f"c{i}"
        gen.enqueue(cid)
        signal = ParleyCancelSignal()
        task: asyncio.Task[object] = asyncio.ensure_future(commander.execute({}, signal=signal))
        await drain()

        kind = i % 5
        if kind == 0:  # success
            transport.deliver("heartbeat", wire.hb(cid))
            transport.deliver("result", wire.res_ok(cid, '{"r":1}'))
        elif kind == 1:  # custom failure
            transport.deliver("result", wire.res_err(cid, "domainErr"))
        elif kind == 2:  # $busy
            transport.deliver("result", wire.res_err(cid, "$busy"))
        elif kind == 3:  # $noAck timeout (never any sign of life)
            clock.advance_by_ms(2600)
        else:  # cancel, then a $cancelled result wins
            signal.abort()
            transport.deliver("result", wire.res_err(cid, "$cancelled"))

        await task
        assert commander._inflight_count == 0  # each cycle terminates before the next
        transport.clear_outbound()
        clock.advance_by_ms(1)

    clock.advance_by_ms(5000)  # drain all tombstones

    assert commander._inflight_count == 0
    assert commander._tombstone_count == 0
    assert commander._live_timer_count == 0
    assert clock.pending_count == 0  # no timer left registered on the clock
    assert _pending_tasks() == []  # no leaked execute task


async def test_handler_sustained_load_returns_to_steady_state() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    config = ParleyConfig(
        heartbeat_interval=1.0,
        result_retry_interval=1.0,
        result_retry_timeout=2.5,
        tombstone_ttl=3.0,
        max_concurrent_executions=4,
        max_active_commands=8,
    )
    # Unwinding handler (not the suppressing stub): every cnl completes the task under cancellation.
    handler = ParkingHandler(transport, TOPICS, config, clock=clock)
    transport.clear_outbound()

    for i in range(CYCLES):
        cid = f"h{i}"
        transport.deliver("request", wire.req(cid))
        await drain()

        kind = i % 6
        if kind in (0, 5):  # complete + rak
            handler.complete(cid, {})
            await drain()
            transport.deliver("request", wire.rak(cid))
        elif kind == 1:  # custom fail + rak
            handler.fail(cid, "domainErr")
            await drain()
            transport.deliver("request", wire.rak(cid))
        elif kind == 2:  # complete, no rak → resultRetryTimeout give-up → tombstone
            handler.complete(cid, {})
            await drain()
            clock.advance_by_ms(2600)
        elif kind == 3:  # cancel executing → unwind → $cancelled + rak
            transport.deliver("request", wire.cnl(cid))
            await drain()
            transport.deliver("request", wire.rak(cid))
        else:  # stateless cancel of an unknown id, then complete this one
            transport.deliver("request", wire.cnl(f"ghost{i}"))
            handler.complete(cid, {})
            await drain()
            transport.deliver("request", wire.rak(cid))

        await drain()
        transport.clear_outbound()
        clock.advance_by_ms(1)

    # $busy bursts: fill the pool, overflow, then drain.
    for burst in range(40):
        parked = []
        for k in range(config.max_active_commands):
            cid = f"b{burst}_{k}"
            transport.deliver("request", wire.req(cid))
            parked.append(cid)
        await drain()
        transport.deliver("request", wire.req(f"overflow{burst}"))  # pool full → stateless $busy
        await drain()
        assert handler._command_count == config.max_active_commands

        remaining = set(parked)
        while remaining:
            progressed = False
            for cid in list(remaining):
                if handler.is_executing(cid):
                    handler.complete(cid, {})
                    await drain()
                    transport.deliver("request", wire.rak(cid))
                    await drain()
                    remaining.discard(cid)
                    progressed = True
            assert progressed  # the drain must not stall
        transport.clear_outbound()
        clock.advance_by_ms(1)

    clock.advance_by_ms(5000)  # drain all tombstones and any lingering result-retry state

    assert handler._command_count == 0
    assert handler._queue_count == 0
    assert handler._executing == 0
    assert handler._tombstone_count == 0
    assert handler._live_timer_count == 0
    assert handler._task_count == 0
    assert clock.pending_count == 0
    assert _pending_tasks() == []
