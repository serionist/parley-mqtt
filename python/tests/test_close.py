"""close() semantics (F5, binding): commander resolves values; handler cancels-and-awaits."""

from __future__ import annotations

import asyncio

import pytest

from parley import ParleyCommander, ParleyConfig
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock


async def test_commander_close_resolves_inflight_with_cancelled_value() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    task_a = await start_call(commander, gen, "A", {"x": 1})
    task_b = await start_call(commander, gen, "B", {"y": 2})
    assert commander._inflight_count == 2

    await commander.close()

    for task in (task_a, task_b):
        result = await task  # resolves as a VALUE — never raises, never task-cancelled
        assert not result.is_success
        assert result.error is not None and result.error.code == "$cancelled"

    assert commander._inflight_count == 0
    assert commander._live_timer_count == 0


async def test_commander_post_close_execute_raises_and_inbound_ignored() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, id_generator=gen, clock=clock
    )
    await commander.close()

    gen.enqueue("A")
    with pytest.raises(RuntimeError, match="closed"):
        await commander.execute({"x": 1})

    # Inbound callbacks after close are silently ignored (no publish, no crash).
    transport.clear_outbound()
    transport.deliver("result", wire.res_ok("A", "null"))
    await drain()
    assert len(transport.outbound) == 0


async def test_commander_close_is_idempotent() -> None:
    transport = FakeTransport()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, clock=VirtualClock()
    )
    await commander.close()
    await commander.close()  # no error


async def test_handler_close_cancels_and_awaits_executing() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    # maxConcurrent 1: A executes, B queues — close must clean up both, no res, no warning.
    handler = ParkingHandler(transport, TOPICS, ParleyConfig(max_active_commands=8), clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    transport.deliver("request", wire.req("B"))
    await drain()
    assert handler._executing == 1  # A executing (serial), B queued
    assert handler._queue_count == 1

    await handler.close()

    assert handler._command_count == 0
    assert handler._queue_count == 0
    assert handler._executing == 0
    assert handler._task_count == 0
    assert handler._live_timer_count == 0
    # No result was published for either command (close drops in-flight outcomes).
    assert transport.publish_count("result", "res") == 0


async def test_handler_close_is_idempotent() -> None:
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=VirtualClock())
    await handler.close()
    await handler.close()  # no error


async def test_handler_close_awaits_even_a_slow_unwind() -> None:
    # A handler that awaits a short cleanup on cancellation still finishes before close() returns.
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    tasks_before = list(handler._tasks)
    assert len(tasks_before) == 1

    await handler.close()
    # The execution task was awaited to completion by close() (cancelled and retrieved).
    assert tasks_before[0].done()
    assert all(isinstance(t, asyncio.Task) for t in tasks_before)
