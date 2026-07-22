"""Tombstone TTL + oldest-first hard cap, both sides (protocol.md C5 / H5 / H8)."""

from __future__ import annotations

from parley import ParleyCommander, ParleyConfig
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock


async def test_handler_tombstone_ttl_evicts() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, ParleyConfig(tombstone_ttl=5.0), clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    handler.complete("A", {"r": 1})
    await drain()
    transport.deliver("request", wire.rak("A"))
    await drain()

    assert handler._command_count == 0
    assert handler._tombstone_count == 1

    clock.advance_to_ms(5001)
    await drain()
    assert handler._tombstone_count == 0
    assert handler._live_timer_count == 0

    await handler.close()


async def test_handler_tombstone_cap_evicts_oldest() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(
        transport, TOPICS, ParleyConfig(max_tombstones=2, tombstone_ttl=1000.0), clock=clock
    )
    transport.clear_outbound()

    for cid in ("A", "B", "C"):
        transport.deliver("request", wire.req(cid))
        await drain()
        handler.complete(cid, {"r": cid})
        await drain()
        transport.deliver("request", wire.rak(cid))
        await drain()

    # cap=2 → the oldest tombstone (A) is evicted; B and C remain.
    assert handler._tombstone_count == 2
    # A is forgotten → a fresh req A re-admits (new hb); B is still tombstoned → ignored.
    transport.clear_outbound()
    transport.deliver("request", wire.req("A"))
    await drain()
    assert transport.publish_count("heartbeat", "hb") == 1
    transport.deliver("request", wire.req("B"))
    await drain()
    assert transport.publish_count("heartbeat", "hb") == 1  # B still tombstoned, ignored

    await handler.close()


async def test_commander_tombstone_ttl_then_late_res_ignored() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, ParleyConfig(tombstone_ttl=5.0), id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    task = await start_call(commander, gen, "A", {"x": 1})
    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    assert (await task).is_success
    assert commander._tombstone_count == 1

    # A late duplicate res while tombstoned → re-emit rak, discard content.
    transport.clear_outbound()
    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    assert transport.publish_count("request", "rak") == 1

    # After the TTL the id is forgotten → a late res elicits nothing.
    clock.advance_to_ms(5001)
    await drain()
    assert commander._tombstone_count == 0
    transport.clear_outbound()
    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    assert len(transport.outbound) == 0
    assert commander._live_timer_count == 0

    await commander.close()


async def test_commander_tombstone_cap_evicts_oldest() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport,
        TOPICS,
        ParleyConfig(max_tombstones=2, tombstone_ttl=1000.0),
        id_generator=gen,
        clock=clock,
    )
    transport.clear_outbound()

    for cid in ("A", "B", "C"):
        task = await start_call(commander, gen, cid, {"x": 1})
        transport.deliver("result", wire.res_ok(cid, '{"r":1}'))
        await drain()
        assert (await task).is_success

    assert commander._tombstone_count == 2  # oldest (A) evicted

    await commander.close()
