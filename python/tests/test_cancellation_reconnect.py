"""Cancellation semantics + reconnect resubscription (protocol.md C7 / H7 / §2)."""

from __future__ import annotations

import asyncio

from parley import (
    EventCancelSignal,
    ParleyCancelSignal,
    ParleyCommander,
    ParleyCommandHandler,
    ParleyExecution,
    ParleyOutcome,
)
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport, SubscribeAction
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock


def _new_commander(
    clock: VirtualClock, transport: FakeTransport, gen: ScriptedIdGenerator, config=None
):
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, config, id_generator=gen, clock=clock
    )
    transport.clear_outbound()
    return commander


async def test_pre_aborted_signal_publishes_cnl_after_req() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander = _new_commander(clock, transport, gen)

    signal = ParleyCancelSignal()
    signal.abort()  # already aborted before the call starts
    task = await start_call(commander, gen, "A", {"x": 1}, signal=signal)

    # Wire order is req-then-cnl (the signal is wired after the initial publish).
    types = [m["t"] for m in transport.publishes("request")]
    assert types == ["req", "cnl"]

    transport.deliver("result", wire.res_err("A", "$cancelled"))
    await drain()
    result = await task
    assert not result.is_success
    assert result.error is not None and result.error.code == "$cancelled"

    await commander.close()


async def test_event_cancel_signal_triggers_cnl_synchronously() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander = _new_commander(clock, transport, gen)

    signal = EventCancelSignal()
    task = await start_call(commander, gen, "A", {"x": 1}, signal=signal)

    # trigger() fires Parley's listener in the same call path AND sets the backing Event — the cnl
    # is published synchronously, with no intervening drain.
    signal.trigger()
    assert transport.publish_count("request", "cnl") == 1
    assert signal.event.is_set()
    assert signal.aborted

    transport.deliver("result", wire.res_err("A", "$cancelled"))
    await drain()
    result = await task
    assert not result.is_success and result.error is not None and result.error.code == "$cancelled"

    await commander.close()


async def test_mid_flight_cancel_yields_cancelled() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    transport.deliver("request", wire.cnl("A"))
    await drain()

    # The ParkingHandler unwinds via CancelledError while aborted → $cancelled via normal completion.
    res = transport.publishes("result")
    assert res == [{"v": 1, "t": "res", "id": "A", "ok": False, "e": "$cancelled"}]

    transport.deliver("request", wire.rak("A"))
    await drain()
    assert handler._command_count == 0
    assert handler._executing == 0
    assert handler._tombstone_count == 1

    await handler.close()


async def test_thrown_cancellederror_while_aborted_maps_to_cancelled() -> None:
    clock = VirtualClock()
    transport = FakeTransport()

    class RaiseOnCancelHandler(ParleyCommandHandler[object, object, object, str]):
        async def handle_command(
            self, request: object, execution: ParleyExecution[object]
        ) -> ParleyOutcome[object, str]:
            try:
                await asyncio.get_running_loop().create_future()  # parks until cancelled
            except asyncio.CancelledError:
                # The thrown-cancellation idiom (try/finally cleanup then re-raise).
                raise
            return ParleyOutcome.success(None)  # pragma: no cover - never reached

    handler = RaiseOnCancelHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    transport.deliver("request", wire.cnl("A"))
    await drain()

    res = transport.publishes("result")
    assert res == [{"v": 1, "t": "res", "id": "A", "ok": False, "e": "$cancelled"}]

    await handler.close()


async def test_external_cancellederror_is_reraised_no_outcome() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()

    # Cancel the internal execution task WITHOUT aborting the signal (cancellation the library did not
    # cause). The wrapper must re-raise (asyncio semantics) — no $cancelled result is produced.
    task = next(iter(handler._tasks))
    task.cancel()
    await drain()

    assert task.cancelled()
    assert transport.publish_count("result", "res") == 0

    await handler.close()


async def test_cancel_on_result_cached_republishes() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    handler.complete("A", {"r": 1})
    await drain()
    transport.clear_outbound()

    # H7: cnl on a result-cached command re-publishes the cached res (cancel lost the race).
    transport.deliver("request", wire.cnl("A"))
    await drain()
    assert transport.publishes("result") == [
        {"v": 1, "t": "res", "id": "A", "ok": True, "p": {"r": 1}}
    ]

    await handler.close()


async def test_commander_cancel_after_terminal_is_noop() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander = _new_commander(clock, transport, gen)

    signal = ParleyCancelSignal()
    task = await start_call(commander, gen, "A", {"x": 1}, signal=signal)
    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    assert (await task).is_success

    # Aborting after the call already went terminal must not publish a cnl.
    transport.clear_outbound()
    signal.abort()
    await drain()
    assert transport.publish_count("request", "cnl") == 0

    await commander.close()


async def test_reconnect_commander_resubscribes_all_topics() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander = _new_commander(clock, transport, gen)

    task = await start_call(commander, gen, "A", {"x": 1})
    transport.clear_outbound()

    transport.fire_connected()
    subscribed = {a.topic for a in transport.outbound if isinstance(a, SubscribeAction)}
    assert subscribed == {"heartbeat", "progress", "result"}

    # In-flight timers are unaffected: the req retry still fires after the reconnect.
    transport.clear_outbound()
    clock.advance_to_ms(2000)
    await drain()
    assert transport.publish_count("request", "req") == 1

    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    assert (await task).is_success
    await commander.close()


async def test_reconnect_handler_resubscribes_request_topic() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    transport.clear_outbound()

    transport.fire_connected()
    subscribed = [a.topic for a in transport.outbound if isinstance(a, SubscribeAction)]
    assert subscribed == ["request"]

    await handler.close()
