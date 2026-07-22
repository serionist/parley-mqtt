"""Optional codec hooks: encode/decode on both sides, with the settled throw semantics (design.md)."""

from __future__ import annotations

import pytest

from parley import (
    ParleyCommander,
    ParleyCommanderCodec,
    ParleyHandlerCodec,
)
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.unit_handlers import SuccessHandler
from support.virtual_clock import VirtualClock


def _commander(clock, transport, gen, codec=None):
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, codec=codec, id_generator=gen, clock=clock
    )
    transport.clear_outbound()
    return commander


async def test_commander_encode_request_transforms_payload() -> None:
    clock, transport, gen = VirtualClock(), FakeTransport(), ScriptedIdGenerator()
    codec: ParleyCommanderCodec[object, object, object] = ParleyCommanderCodec(
        encode_request=lambda v: {"wrapped": v}
    )
    commander = _commander(clock, transport, gen, codec)

    task = await start_call(commander, gen, "A", {"x": 1})
    assert transport.publishes("request")[0]["p"] == {"wrapped": {"x": 1}}

    transport.deliver("result", wire.res_ok("A", "null"))
    await drain()
    await task
    await commander.close()


async def test_commander_decode_result_transforms_value() -> None:
    clock, transport, gen = VirtualClock(), FakeTransport(), ScriptedIdGenerator()
    codec: ParleyCommanderCodec[object, object, object] = ParleyCommanderCodec(
        decode_result=lambda raw: {"decoded": raw}
    )
    commander = _commander(clock, transport, gen, codec)

    task = await start_call(commander, gen, "A", {"x": 1})
    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()
    result = await task
    assert result.is_success
    assert result.value == {"decoded": {"r": 1}}
    await commander.close()


async def test_commander_decode_progress_throw_drops_but_keeps_sign_of_life() -> None:
    import asyncio

    clock, transport, gen = VirtualClock(), FakeTransport(), ScriptedIdGenerator()

    def boom(_raw: object) -> object:
        raise ValueError("bad progress")

    codec: ParleyCommanderCodec[object, object, object] = ParleyCommanderCodec(decode_progress=boom)
    commander = _commander(clock, transport, gen, codec)

    received: list[object] = []
    gen.enqueue("A")
    task = asyncio.ensure_future(commander.execute({"x": 1}, on_progress=received.append))
    await drain()

    transport.deliver("progress", wire.prg("A", '{"step":1}'))
    await drain()
    assert received == []  # progress dropped (decode threw); no callback fired

    # Still a sign of life: the command remains in flight and completes normally.
    transport.deliver("result", wire.res_ok("A", "null"))
    await drain()
    assert (await task).is_success
    await commander.close()


async def test_commander_progress_decode_passthrough_to_sink() -> None:
    import asyncio

    clock, transport, gen = VirtualClock(), FakeTransport(), ScriptedIdGenerator()
    codec: ParleyCommanderCodec[object, object, object] = ParleyCommanderCodec(
        decode_progress=lambda raw: {"decoded": raw}
    )
    commander = _commander(clock, transport, gen, codec)

    received: list[object] = []
    gen.enqueue("A")
    task = asyncio.ensure_future(commander.execute({"x": 1}, on_progress=received.append))
    await drain()

    transport.deliver("progress", wire.prg("A", '{"step":1}'))
    await drain()
    assert received == [{"decoded": {"step": 1}}]

    transport.deliver("result", wire.res_ok("A", "null"))
    await drain()
    await task
    await commander.close()


async def test_commander_encode_request_throw_is_fail_fast() -> None:
    clock, transport, gen = VirtualClock(), FakeTransport(), ScriptedIdGenerator()

    def boom(_v: object) -> object:
        raise ValueError("bad request")

    codec: ParleyCommanderCodec[object, object, object] = ParleyCommanderCodec(encode_request=boom)
    commander = _commander(clock, transport, gen, codec)

    gen.enqueue("A")
    with pytest.raises(ValueError, match="bad request"):
        await commander.execute({"x": 1})

    # No command state and no publish resulted from the failed encode.
    assert commander._inflight_count == 0
    assert transport.publish_count("request", "req") == 0
    await commander.close()


async def test_handler_decode_request_transforms() -> None:
    clock, transport = VirtualClock(), FakeTransport()
    codec: ParleyHandlerCodec[object, object, object] = ParleyHandlerCodec(
        decode_request=lambda raw: raw["inner"]  # type: ignore[index]
    )
    handler = SuccessHandler(transport, TOPICS, clock=clock, codec=codec)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A", '{"inner":5}'))
    await drain()
    # SuccessHandler echoes the (decoded) request → res p == 5.
    res = [m for m in transport.publishes("result") if m.get("t") == "res"]
    assert res == [{"v": 1, "t": "res", "id": "A", "ok": True, "p": 5}]
    await handler.close()


async def test_handler_decode_request_throw_drops_no_admission() -> None:
    clock, transport = VirtualClock(), FakeTransport()

    def boom(_raw: object) -> object:
        raise ValueError("bad")

    codec: ParleyHandlerCodec[object, object, object] = ParleyHandlerCodec(decode_request=boom)
    handler = SuccessHandler(transport, TOPICS, clock=clock, codec=codec)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A", '{"x":1}'))
    await drain()
    assert transport.publish_count("heartbeat", "hb") == 0  # dropped, no admission
    assert handler._command_count == 0
    await handler.close()


async def test_handler_encode_result_transforms() -> None:
    clock, transport = VirtualClock(), FakeTransport()
    codec: ParleyHandlerCodec[object, object, object] = ParleyHandlerCodec(
        encode_result=lambda v: {"out": v}
    )
    handler = SuccessHandler(transport, TOPICS, clock=clock, result=42, codec=codec)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    res = [m for m in transport.publishes("result") if m.get("t") == "res"]
    assert res == [{"v": 1, "t": "res", "id": "A", "ok": True, "p": {"out": 42}}]
    await handler.close()
