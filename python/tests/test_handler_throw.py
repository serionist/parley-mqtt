"""Thrown handler and unserializable outcome both abort — never a leak (design.md contract)."""

from __future__ import annotations

from parley import ParleyCommandHandler, ParleyExecution, ParleyOutcome
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS
from support.unit_handlers import ThrowingHandler
from support.virtual_clock import VirtualClock


async def test_thrown_handler_aborts_command() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ThrowingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()

    # hb published on admission, then the handler raised → abort (slot freed, id tombstoned, no res).
    assert transport.publish_count("heartbeat", "hb") == 1
    assert transport.publish_count("result", "res") == 0
    assert handler._command_count == 0
    assert handler._executing == 0
    assert handler._tombstone_count == 1
    assert handler._task_count == 0

    await handler.close()


async def test_unserializable_result_aborts_command() -> None:
    clock = VirtualClock()
    transport = FakeTransport()

    class Unserializable:
        pass

    class BadResultHandler(ParleyCommandHandler[object, object, object, str]):
        async def handle_command(
            self, request: object, execution: ParleyExecution[object]
        ) -> ParleyOutcome[object, str]:
            return ParleyOutcome.success(Unserializable())  # json.dumps will raise

    handler = BadResultHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()

    # Outcome-encode failure is treated exactly like a thrown handler — abort, not a leak.
    assert transport.publish_count("result", "res") == 0
    assert handler._command_count == 0
    assert handler._tombstone_count == 1
    assert handler._task_count == 0

    await handler.close()


async def test_second_command_dispatches_after_abort() -> None:
    # A thrown handler frees the slot so a queued/next command still runs (serial handler).
    clock = VirtualClock()
    transport = FakeTransport()
    handler = ThrowingHandler(transport, TOPICS, clock=clock)
    transport.clear_outbound()

    transport.deliver("request", wire.req("A"))
    await drain()
    transport.deliver("request", wire.req("B"))
    await drain()

    # Both admitted (each hb), both threw → both aborted, both tombstoned, nothing stuck executing.
    assert handler._executing == 0
    assert handler._command_count == 0
    assert handler._tombstone_count == 2

    await handler.close()
