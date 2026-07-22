"""Reusable handler stubs for the unit suite.

Unlike the vector :class:`~support.vector_handler.VectorHandler`, these handlers actually **complete
under cancellation** — the plan requires close()/cancel unit tests to use handlers that unwind (or
return) on cancel, never the suppressing stub, so ``close()``'s await-the-task never hangs.
"""

from __future__ import annotations

import asyncio

from parley import (
    ParleyCommandHandler,
    ParleyConfig,
    ParleyExecution,
    ParleyOutcome,
    ParleyTopics,
)
from parley.clock import ParleyClock
from parley.handler import ParleyHandlerCodec

from .fake_transport import FakeTransport


class SuccessHandler(ParleyCommandHandler[object, object, object, str]):
    """Returns a fixed success outcome immediately (or echoes the request when none is given)."""

    def __init__(
        self,
        transport: FakeTransport,
        topics: ParleyTopics,
        config: ParleyConfig | None = None,
        *,
        clock: ParleyClock | None = None,
        result: object = None,
        codec: ParleyHandlerCodec[object, object, object] | None = None,
    ) -> None:
        super().__init__(transport, topics, config, codec, clock=clock)
        self._result = result

    async def handle_command(
        self, request: object, execution: ParleyExecution[object]
    ) -> ParleyOutcome[object, str]:
        return ParleyOutcome.success(self._result if self._result is not None else request)


class ThrowingHandler(ParleyCommandHandler[object, object, object, str]):
    """Raises inside the handler body (a programming error) — the command must abort, never leak."""

    async def handle_command(
        self, request: object, execution: ParleyExecution[object]
    ) -> ParleyOutcome[object, str]:
        raise ValueError("handler boom")


class FailHandler(ParleyCommandHandler[object, object, object, str]):
    """Returns a custom-error outcome with the configured code/message."""

    def __init__(
        self,
        transport: FakeTransport,
        topics: ParleyTopics,
        config: ParleyConfig | None = None,
        *,
        clock: ParleyClock | None = None,
        code: str = "customErr",
        message: str | None = None,
    ) -> None:
        super().__init__(transport, topics, config, clock=clock)
        self._code = code
        self._message = message

    async def handle_command(
        self, request: object, execution: ParleyExecution[object]
    ) -> ParleyOutcome[object, str]:
        return ParleyOutcome.error(self._code, self._message)


class ParkingHandler(ParleyCommandHandler[object, object, object, str]):
    """Parks each admitted command on a future; **does not** suppress ``CancelledError``.

    A wire ``cnl`` (or ``close()``) that cancels the task lets the ``CancelledError`` propagate
    (unwind), so the task completes under cancellation — the wrapper then maps a library-initiated
    cancel to ``$cancelled`` or re-raises an external one. Resolve normally via :meth:`complete` /
    :meth:`fail`.
    """

    def __init__(
        self,
        transport: FakeTransport,
        topics: ParleyTopics,
        config: ParleyConfig | None = None,
        *,
        clock: ParleyClock | None = None,
    ) -> None:
        super().__init__(transport, topics, config, clock=clock)
        self._pending: dict[str, asyncio.Future[ParleyOutcome[object, str]]] = {}

    async def handle_command(
        self, request: object, execution: ParleyExecution[object]
    ) -> ParleyOutcome[object, str]:
        loop = asyncio.get_running_loop()
        future: asyncio.Future[ParleyOutcome[object, str]] = loop.create_future()
        self._pending[execution.command_id] = future
        try:
            return await future
        finally:
            # Self-clean on completion OR cancellation-unwind, so _pending never leaks entries.
            self._pending.pop(execution.command_id, None)

    def complete(self, command_id: str, payload: object) -> None:
        future = self._pending.get(command_id)
        if future is not None and not future.done():
            future.set_result(ParleyOutcome.success(payload))

    def fail(self, command_id: str, code: str, message: str | None = None) -> None:
        future = self._pending.get(command_id)
        if future is not None and not future.done():
            future.set_result(ParleyOutcome.error(code, message))

    def is_executing(self, command_id: str) -> bool:
        return command_id in self._pending
