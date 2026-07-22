"""Tri-state execution cap: config math.inf, per-call math.inf, and a positive control (F6)."""

from __future__ import annotations

import math

from parley import ParleyBuiltinError, ParleyCallOptions, ParleyCommander, ParleyConfig
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.virtual_clock import VirtualClock


def _commander(clock, transport, gen, config):
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, config, id_generator=gen, clock=clock
    )
    transport.clear_outbound()
    return commander


async def test_positive_execution_cap_trips() -> None:
    # First sign of life at 1000ms → cap at 1000+8000=9000ms (stale window is generous).
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander = _commander(
        clock, transport, gen, ParleyConfig(execution_timeout=8.0, stale_timeout=100.0)
    )
    task = await start_call(commander, gen, "A", {"x": 1})

    clock.advance_to_ms(1000)
    transport.deliver("heartbeat", wire.hb("A"))
    await drain()
    clock.advance_to_ms(9000)
    await drain()

    result = await task
    assert not result.is_success
    assert result.error is not None and result.error.builtin is ParleyBuiltinError.EXECUTION_TIMEOUT
    await commander.close()


async def test_config_inf_disables_execution_cap() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander = _commander(
        clock, transport, gen, ParleyConfig(execution_timeout=math.inf, stale_timeout=100.0)
    )
    task = await start_call(commander, gen, "A", {"x": 1})

    clock.advance_to_ms(1000)
    transport.deliver("heartbeat", wire.hb("A"))
    await drain()
    # Advance far past the default 60s cap — with the cap disabled, nothing terminates.
    clock.advance_to_ms(70_000)
    await drain()
    assert commander._inflight_count == 1

    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    assert (await task).is_success
    await commander.close()


async def test_per_call_inf_overrides_configured_cap() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    # Config WOULD cap at 8s, but the per-call option disables it for this call.
    commander = _commander(
        clock, transport, gen, ParleyConfig(execution_timeout=8.0, stale_timeout=100.0)
    )
    task = await start_call(
        commander, gen, "A", {"x": 1}, options=ParleyCallOptions(execution_timeout=math.inf)
    )

    clock.advance_to_ms(1000)
    transport.deliver("heartbeat", wire.hb("A"))
    await drain()
    clock.advance_to_ms(70_000)
    await drain()
    assert commander._inflight_count == 1  # no cap tripped despite the configured 8s default

    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    assert (await task).is_success
    await commander.close()
