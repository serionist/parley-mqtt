"""Throwing-adapter containment + self-heal, and contained consumer callbacks (CR4 / design.md)."""

from __future__ import annotations

import asyncio

from parley import ParleyCommander, ParleyConfig
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock


async def test_handler_heartbeat_survives_throwing_adapter() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, ParleyConfig(heartbeat_interval=3.0), clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    transport.clear_outbound()

    transport.fail_publish = True
    clock.advance_to_ms(3000)  # hb fires; publish raises; contained (no escape)
    await drain()
    assert transport.publish_count("heartbeat", "hb") == 0  # lost, but the cadence is not killed

    transport.fail_publish = False
    clock.advance_to_ms(6000)  # next hb self-heals
    await drain()
    assert transport.publish_count("heartbeat", "hb") == 1
    assert handler._live_timer_count == 1  # the single heartbeat timer, still alive

    await handler.close()


async def test_commander_request_retry_survives_throwing_adapter() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, ParleyConfig(request_retry_interval=2.0), id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    task = await start_call(commander, gen, "A", {"x": 1})
    transport.clear_outbound()

    transport.fail_publish = True
    clock.advance_to_ms(2000)  # req retry fires; publish raises; contained
    await drain()
    assert transport.publish_count("request", "req") == 0

    transport.fail_publish = False
    clock.advance_to_ms(4000)  # retry self-heals
    await drain()
    assert transport.publish_count("request", "req") == 1

    transport.deliver("result", wire.res_ok("A", "null"))
    await drain()
    assert (await task).is_success
    await commander.close()


async def test_on_progress_exception_is_contained() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    def boom(_p: object) -> None:
        raise ValueError("consumer progress boom")

    gen.enqueue("A")
    task = asyncio.ensure_future(commander.execute({"x": 1}, on_progress=boom))
    await drain()

    transport.deliver("progress", wire.prg("A", '{"step":1}'))
    await drain()  # the throwing sink must not escape onto the library path

    transport.deliver("result", wire.res_ok("A", "null"))
    await drain()
    assert (await task).is_success  # command survived the throwing progress callback
    await commander.close()
