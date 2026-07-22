"""Error typing: builtin vs custom, $decodeError, and $-prefix fail-fast (protocol.md §5, C4)."""

from __future__ import annotations

from parley import (
    ParleyBuiltinError,
    ParleyCommander,
    ParleyCommanderCodec,
    ParleyCommandHandler,
    ParleyExecution,
    ParleyOutcome,
)
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS, start_call
from support.scripted_id_generator import ScriptedIdGenerator
from support.virtual_clock import VirtualClock


async def test_custom_error_is_typed_with_message() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    task = await start_call(commander, gen, "A", {"x": 1})
    transport.deliver("result", b'{"v":1,"t":"res","id":"A","ok":false,"e":"domainErr","m":"nope"}')
    await drain()
    result = await task
    assert not result.is_success
    err = result.error
    assert err is not None
    assert err.code == "domainErr"
    assert err.custom == "domainErr"
    assert err.builtin is None
    assert not err.is_builtin
    assert err.message == "nope"

    await commander.close()


async def test_builtin_error_is_typed() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    task = await start_call(commander, gen, "A", {"x": 1})
    transport.deliver("result", wire.res_err("A", "$busy"))
    await drain()
    result = await task
    assert not result.is_success
    err = result.error
    assert err is not None
    assert err.code == "$busy"
    assert err.builtin is ParleyBuiltinError.BUSY
    assert err.is_builtin
    # $busy is exempt from rak (C4).
    assert transport.publish_count("request", "rak") == 0

    await commander.close()


async def test_decode_error_still_raks() -> None:
    def boom(_raw: object) -> object:
        raise ValueError("cannot decode")

    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    codec: ParleyCommanderCodec[object, object, object] = ParleyCommanderCodec(decode_result=boom)
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport, TOPICS, codec=codec, id_generator=gen, clock=clock
    )
    transport.clear_outbound()

    task = await start_call(commander, gen, "A", {"x": 1})
    transport.deliver("result", wire.res_ok("A", '{"r":1}'))
    await drain()

    # C4: a success payload that fails to decode is still rak'd, then terminates $decodeError.
    assert transport.publish_count("request", "rak") == 1
    result = await task
    assert not result.is_success
    assert result.error is not None and result.error.builtin is ParleyBuiltinError.DECODE_ERROR

    await commander.close()


async def test_handler_dollar_prefixed_custom_code_aborts() -> None:
    clock = VirtualClock()
    transport = FakeTransport()

    class BadCodeHandler(ParleyCommandHandler[object, object, object, str]):
        async def handle_command(
            self, request: object, execution: ParleyExecution[object]
        ) -> ParleyOutcome[object, str]:
            return ParleyOutcome.error("$sneaky")  # reserved prefix — must fail-fast to abort

    handler = BadCodeHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()

    # The $-reserved code aborts the command like a thrown handler: hb only, no res published.
    assert transport.publish_count("heartbeat", "hb") == 1
    assert transport.publish_count("result", "res") == 0
    assert handler._command_count == 0
    assert handler._executing == 0
    assert handler._tombstone_count == 1

    await handler.close()


async def test_handler_non_string_custom_code_aborts() -> None:
    clock = VirtualClock()
    transport = FakeTransport()

    class NonStringCodeHandler(ParleyCommandHandler[object, object, object, str]):
        async def handle_command(
            self, request: object, execution: ParleyExecution[object]
        ) -> ParleyOutcome[object, str]:
            return ParleyOutcome.error(123)  # type: ignore[arg-type]

    handler = NonStringCodeHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    assert transport.publish_count("result", "res") == 0
    assert handler._command_count == 0

    await handler.close()
