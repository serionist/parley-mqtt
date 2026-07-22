"""Cancellation signals and the handler execution context (protocol.md H4/H7, design.md).

The commander's ``execute(signal=...)`` accepts any object satisfying :class:`ParleyCancelSignalProtocol`:
an ``aborted`` property plus **synchronous** fire-on-abort listener registration — the listener runs
in the same call stack as the abort, so the wire-level ``cnl`` publishes synchronously with no watcher
task. :class:`asyncio.Event`/:class:`asyncio.Future` do NOT satisfy this (no ``aborted``, no synchronous
hook); the library ships :class:`ParleyCancelSignal` plus :class:`EventCancelSignal` for Event-driven
consumers.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Callable
from typing import Generic, Protocol, TypeVar, runtime_checkable

TProgress = TypeVar("TProgress")


@runtime_checkable
class ParleyCancelSignalProtocol(Protocol):
    """The structural type ``ParleyCommander.execute`` accepts for cooperative cancellation."""

    @property
    def aborted(self) -> bool:
        """Whether cancellation has been requested."""
        ...

    def add_listener(self, listener: Callable[[], None]) -> None:
        """Register a fire-on-abort listener; it runs synchronously in the abort's call stack."""
        ...

    def remove_listener(self, listener: Callable[[], None]) -> None:
        """Deregister a previously registered listener (used by the commander on teardown)."""
        ...


class ParleyCancelSignal:
    """The standard :class:`ParleyCancelSignalProtocol` implementation.

    :meth:`abort` fires every registered listener synchronously, in its own call stack; the commander
    thus publishes ``cnl`` synchronously (no watcher task). Also used internally by the handler as the
    per-command cancellation source handed to the consumer via :attr:`ParleyExecution.signal`.
    """

    __slots__ = ("_aborted", "_listeners")

    def __init__(self) -> None:
        self._aborted = False
        self._listeners: list[Callable[[], None]] = []

    @property
    def aborted(self) -> bool:
        return self._aborted

    def add_listener(self, listener: Callable[[], None]) -> None:
        self._listeners.append(listener)

    def remove_listener(self, listener: Callable[[], None]) -> None:
        with contextlib.suppress(ValueError):
            self._listeners.remove(listener)

    def abort(self) -> None:
        """Request cancellation, firing every registered listener synchronously. Idempotent."""
        if self._aborted:
            return
        self._aborted = True
        listeners = self._listeners
        self._listeners = []
        for listener in listeners:
            # Containment: a consumer abort listener must never escape onto library paths.
            with contextlib.suppress(Exception):
                listener()


class EventCancelSignal:
    """A :class:`ParleyCancelSignalProtocol` adapter backed by an :class:`asyncio.Event`.

    :meth:`trigger` both sets the Event (waking any awaiter) and fires Parley's synchronous listeners
    in the same call path — so ``cnl`` still publishes synchronously. Pass the instance as
    ``execute(signal=...)`` and call :meth:`trigger` to cancel.
    """

    __slots__ = ("_event", "_signal")

    def __init__(self, event: asyncio.Event | None = None) -> None:
        self._event = event if event is not None else asyncio.Event()
        self._signal = ParleyCancelSignal()

    @property
    def event(self) -> asyncio.Event:
        """The backing Event (set when :meth:`trigger` fires)."""
        return self._event

    @property
    def aborted(self) -> bool:
        return self._signal.aborted

    def add_listener(self, listener: Callable[[], None]) -> None:
        self._signal.add_listener(listener)

    def remove_listener(self, listener: Callable[[], None]) -> None:
        self._signal.remove_listener(listener)

    def trigger(self) -> None:
        """Cancel: fire Parley's synchronous listeners, then set the backing Event."""
        self._signal.abort()
        self._event.set()


class ParleyExecution(Generic[TProgress]):
    """The execution context handed to a handler (protocol.md H4/H7/C8).

    Exposes the command id, a cancellation ``signal`` triggered by a wire-level ``cnl``, and a
    progress emitter (each call publishes one ``prg``; delivery is advisory).
    """

    __slots__ = ("_emit", "command_id", "signal")

    def __init__(
        self,
        command_id: str,
        signal: ParleyCancelSignalProtocol,
        emit: Callable[[TProgress], None],
    ) -> None:
        self.command_id = command_id
        self.signal = signal
        self._emit = emit

    def report_progress(self, progress: TProgress) -> None:
        """Emit one advisory progress update (protocol.md C8; never acknowledged)."""
        self._emit(progress)
