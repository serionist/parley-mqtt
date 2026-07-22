"""Synchronous-delivery (loopback) regressions — "publish comes last", no orphaned self-rearming
timers (design.md; the three sites the TS review found)."""

from __future__ import annotations

import json

from parley import ParleyCancelSignal, ParleyCommander
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.unit_handlers import ParkingHandler
from support.virtual_clock import VirtualClock


def _t(payload: bytes) -> str:
    return json.loads(payload.decode("utf-8"))["t"]


async def test_hb_from_req_reentrant_enters_live_no_orphan() -> None:
    # A synchronous hb delivered from inside the req publish enters LIVE re-entrantly; the request-retry
    # and sign-of-life timers (scheduled BEFORE the publish) are found, so no req-storm is orphaned.
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    def loopback(topic: str, payload: bytes) -> None:
        if _t(payload) == "req":
            transport.deliver("heartbeat", wire.hb(json.loads(payload)["id"]))

    transport.on_publish = loopback
    task = await start_call(commander, gen, "A", {"x": 1})

    # Entered LIVE: advancing past the request-retry interval produces NO further req (retries stopped).
    transport.clear_outbound()
    clock.advance_to_ms(10_000)
    await drain()
    assert transport.publish_count("request", "req") == 0

    transport.on_publish = None
    transport.deliver("result", wire.res_ok("A", "null"))
    await drain()
    assert (await task).is_success
    assert commander._live_timer_count == 1  # tombstone only — nothing orphaned
    await commander.close()


async def test_cancelled_from_cnl_reentrant_teardown_no_orphan() -> None:
    # A synchronous $cancelled res delivered from inside the cnl publish tears the command down
    # re-entrantly; the cancel-retry timer (scheduled BEFORE the publish) is found — no cnl-storm.
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    def loopback(topic: str, payload: bytes) -> None:
        if _t(payload) == "cnl":
            transport.deliver("result", wire.res_err(json.loads(payload)["id"], "$cancelled"))

    transport.on_publish = loopback

    signal = ParleyCancelSignal()
    task = await start_call(commander, gen, "A", {"x": 1}, signal=signal)
    signal.abort()  # publishes cnl → loopback delivers $cancelled synchronously → re-entrant teardown
    await drain()

    result = await task
    assert not result.is_success and result.error is not None and result.error.code == "$cancelled"

    transport.on_publish = None
    transport.clear_outbound()
    clock.advance_to_ms(10_000)
    await drain()
    assert transport.publish_count("request", "cnl") == 0  # no self-rearming cnl storm
    assert commander._live_timer_count == 1  # tombstone only
    await commander.close()


async def test_rak_from_res_reentrant_tombstone_no_orphan() -> None:
    # A synchronous rak delivered from inside the res publish moves the command to a tombstone
    # re-entrantly; the result-retry timers (scheduled BEFORE the publish) are found — no res-storm.
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ParkingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    def loopback(topic: str, payload: bytes) -> None:
        if _t(payload) == "res":
            transport.deliver("request", wire.rak(json.loads(payload)["id"]))

    transport.on_publish = loopback

    transport.deliver("request", wire.req("A"))
    await drain()
    handler.complete("A", {"r": 1})  # res publish → loopback rak → re-entrant tombstone
    await drain()

    assert handler._command_count == 0
    assert handler._tombstone_count == 1

    transport.on_publish = None
    transport.clear_outbound()
    clock.advance_to_ms(10_000)
    await drain()
    assert transport.publish_count("result", "res") == 0  # no self-rearming res storm
    assert handler._live_timer_count == 1  # tombstone only
    assert handler._task_count == 0

    await handler.close()
