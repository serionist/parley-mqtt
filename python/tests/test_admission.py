"""Admission / dedup / capacity + atomic re-entrancy (protocol.md H1/H2/H8, docs/testing.md)."""

from __future__ import annotations

from parley import ParleyConfig
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock


async def test_dedup_precedes_capacity_and_reentrancy() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, ParleyConfig(max_active_commands=1), clock=clock)
    transport.clear_outbound()

    # Two synchronous duplicate deliveries of A must not double-admit (dedup + capacity are atomic).
    transport.deliver("request", wire.req("A"))
    transport.deliver("request", wire.req("A"))
    await drain()

    assert transport.publish_count("heartbeat", "hb") == 2  # admission hb + dedup hb
    assert handler._command_count == 1
    assert handler._executing == 1

    # B is unknown and the pool is full → stateless $busy (no state created).
    transport.deliver("request", wire.req("B"))
    await drain()
    busy = [m for m in transport.publishes("result") if m.get("e") == "$busy"]
    assert len(busy) == 1
    assert handler._command_count == 1  # B created no state

    # A duplicate of the admitted A still gets hb, never $busy, even while full.
    transport.clear_outbound()
    transport.deliver("request", wire.req("A"))
    await drain()
    assert transport.publish_count("heartbeat", "hb") == 1
    assert transport.publish_count("result", "res") == 0

    await handler.close()


async def test_admission_publishes_first_heartbeat_immediately() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    hbs = transport.publishes("heartbeat")
    assert hbs == [{"v": 1, "t": "hb", "id": "A"}]

    await handler.close()


async def test_full_pool_reemits_busy_per_duplicate() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, ParleyConfig(max_active_commands=1), clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    transport.clear_outbound()

    for _ in range(3):
        transport.deliver("request", wire.req("B"))
        await drain()

    busy = [m for m in transport.publishes("result") if m.get("e") == "$busy"]
    assert len(busy) == 3
    assert handler._command_count == 1

    await handler.close()
