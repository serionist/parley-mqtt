"""A scripted, time-driven handler stub for vector/unit replay (mirrors the TS ``VectorHandler``).

Each admitted command's :meth:`handle_command` parks on a future; the runner resolves it when the
scenario's ``handler`` action fires. The stub is **non-reactive**: it suppresses the library-injected
:class:`asyncio.CancelledError` (a wire ``cnl`` on an executing command) and re-parks awaiting its
script — modelling a handler that only responds via the script, which vectors 12/27 depend on. This
suppression is scoped to vector/unit REPLAY only; ``close()`` and bounded-state cancel tests use
handlers that actually complete under cancellation.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass

from parley import (
    ParleyBuiltinError,
    ParleyCommandHandler,
    ParleyConfig,
    ParleyExecution,
    ParleyOutcome,
    ParleyTopics,
)
from parley.clock import ParleyClock

from .fake_transport import FakeTransport


@dataclass
class _Pending:
    future: asyncio.Future[ParleyOutcome[object, str]]
    execution: ParleyExecution[object]


class VectorHandler(ParleyCommandHandler[object, object, object, str]):
    """The suppressing scripted handler stub used by the vector/unit runners."""

    def __init__(
        self,
        transport: FakeTransport,
        topics: ParleyTopics,
        config: ParleyConfig,
        clock: ParleyClock,
    ) -> None:
        super().__init__(transport, topics, config, clock=clock)
        self._pending: dict[str, _Pending] = {}

    async def handle_command(
        self, request: object, execution: ParleyExecution[object]
    ) -> ParleyOutcome[object, str]:
        loop = asyncio.get_running_loop()
        while True:
            future: asyncio.Future[ParleyOutcome[object, str]] = loop.create_future()
            self._pending[execution.command_id] = _Pending(future, execution)
            try:
                return await future
            except asyncio.CancelledError:
                # Non-reactive stub: suppress the library-injected cancel and re-park awaiting the
                # script. External cancellation (signal not aborted) re-raises per asyncio semantics.
                if execution.signal.aborted:
                    continue
                raise

    def complete(self, command_id: str, payload: object) -> None:
        self._resolve(command_id, ParleyOutcome.success(payload))

    def fail(self, command_id: str, code: str, message: str | None = None) -> None:
        outcome: ParleyOutcome[object, str] = (
            ParleyOutcome.cancelled()
            if code == ParleyBuiltinError.CANCELLED.value
            else ParleyOutcome.error(code, message)
        )
        self._resolve(command_id, outcome)

    def emit_progress(self, command_id: str, payload: object) -> None:
        self._peek(command_id).execution.report_progress(payload)

    def is_executing(self, command_id: str) -> bool:
        return command_id in self._pending

    def is_cancel_requested(self, command_id: str) -> bool:
        return self._peek(command_id).execution.signal.aborted

    def has_pending(self) -> bool:
        return bool(self._pending)

    def abandon_pending(self) -> None:
        """Resolve every still-parked command with ``$cancelled`` so its task completes.

        Used by the runner at teardown so no executing scripted handler is left parked on a future
        when the event loop closes (which would emit a "Task was destroyed but it is pending" warning).
        """
        for pending in list(self._pending.values()):
            if not pending.future.done():
                pending.future.set_result(ParleyOutcome.cancelled())
        self._pending.clear()

    def _resolve(self, command_id: str, outcome: ParleyOutcome[object, str]) -> None:
        pending = self._pending.pop(command_id, None)
        if pending is None:
            raise AssertionError(f"No admitted command '{command_id}' is executing.")
        if not pending.future.done():
            pending.future.set_result(outcome)

    def _peek(self, command_id: str) -> _Pending:
        pending = self._pending.get(command_id)
        if pending is None:
            raise AssertionError(f"No admitted command '{command_id}' is executing.")
        return pending
