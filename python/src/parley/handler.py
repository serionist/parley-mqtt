"""The callee side of one command type (protocol.md §7, rules H1–H8).

The consumer overrides :meth:`ParleyCommandHandler.handle_command`. A topic set MUST have at most one
active handler. Admission (dedup lookup, capacity check, slot reservation, first heartbeat) is one
atomic synchronous step — natural on a single event loop, since all state is mutated before any
publish, so a synchronous re-entrant delivery observes a consistent table and dedup catches the
duplicate.

Cancellation model (binding, per the approved plan): a wire ``cnl`` on an EXECUTING command aborts the
per-command cancellation signal AND cancels the handler-execution task (``task.cancel()`` →
:class:`asyncio.CancelledError`). The wrapper catches ``CancelledError``: if the signal is aborted
(library-initiated) it yields a ``$cancelled`` result via normal completion (cached + retried per H5);
if not (external cancellation) it re-raises. A non-cancellation exception aborts the command like a
thrown handler — never a leak.
"""

from __future__ import annotations

import asyncio
import contextlib
from abc import ABC, abstractmethod
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass
from typing import Generic, TypeVar

from ._envelope import PROTOCOL_VERSION, MessageType, is_valid_id, parse, serialize, to_payload
from ._runtime import ParleyRuntime, ParleyTimer
from ._tombstones import TombstoneTable
from .adapter import ParleyConnectionAdapter
from .clock import LoopClock, ParleyClock
from .config import ParleyConfig, ParleyTopics
from .errors import ParleyBuiltinError, ParleyOutcome, ParleyOutcomeKind
from .execution import ParleyCancelSignal, ParleyExecution

TRequest = TypeVar("TRequest")
TProgress = TypeVar("TProgress")
TResult = TypeVar("TResult")
TError = TypeVar("TError")

_QUEUED = "queued"
_EXECUTING = "executing"
_RESULT_CACHED = "result-cached"


@dataclass(frozen=True, slots=True)
class ParleyHandlerCodec(Generic[TRequest, TResult, TProgress]):
    """Optional per-handler codec hooks (design.md). All default to a raw pass-through.

    ``decode_request`` throwing drops the request silently — no admission (§4). ``encode_result`` and
    ``encode_progress`` are the encode side; an ``encode_result`` throw aborts the command like a
    thrown handler.
    """

    decode_request: Callable[[object], TRequest] | None = None
    encode_result: Callable[[TResult], object] | None = None
    encode_progress: Callable[[TProgress], object] | None = None


class _HandlerCommand(Generic[TRequest, TProgress]):
    __slots__ = (
        "abort",
        "cached_heartbeat_bytes",
        "cached_result_bytes",
        "execution",
        "heartbeat_timer",
        "id",
        "request",
        "result_retry_timeout_timer",
        "result_retry_timer",
        "state",
        "task",
    )

    def __init__(
        self,
        command_id: str,
        request: TRequest,
        abort: ParleyCancelSignal,
        execution: ParleyExecution[TProgress],
    ) -> None:
        self.id = command_id
        self.request = request
        self.abort = abort
        self.execution = execution
        self.state = _QUEUED
        self.cached_heartbeat_bytes: bytes | None = None
        self.cached_result_bytes: bytes | None = None
        self.heartbeat_timer: ParleyTimer | None = None
        self.result_retry_timer: ParleyTimer | None = None
        self.result_retry_timeout_timer: ParleyTimer | None = None
        self.task: asyncio.Task[None] | None = None


class ParleyCommandHandler(ABC, Generic[TRequest, TProgress, TResult, TError]):
    """The callee side of one command type (protocol.md §7, rules H1–H8)."""

    __slots__ = (
        "_adapter",
        "_codec",
        "_commands",
        "_config",
        "_dispatching",
        "_disposed",
        "_executing_count",
        "_queue",
        "_runtime",
        "_tasks",
        "_tombstones",
        "_topics",
    )

    def __init__(
        self,
        adapter: ParleyConnectionAdapter,
        topics: ParleyTopics,
        config: ParleyConfig | None = None,
        codec: ParleyHandlerCodec[TRequest, TResult, TProgress] | None = None,
        *,
        clock: ParleyClock | None = None,
    ) -> None:
        self._adapter = adapter
        self._topics = topics
        self._config = config if config is not None else ParleyConfig()
        self._runtime = ParleyRuntime(clock if clock is not None else LoopClock())
        self._tombstones = TombstoneTable(self._runtime, self._config.max_tombstones)
        self._codec = codec
        self._commands: dict[str, _HandlerCommand[TRequest, TProgress]] = {}
        self._queue: deque[_HandlerCommand[TRequest, TProgress]] = deque()
        self._tasks: set[asyncio.Task[None]] = set()
        self._executing_count = 0
        self._dispatching = False
        self._disposed = False

        self._adapter.on_connected(self._on_connected)
        self._subscribe_all()

    @abstractmethod
    async def handle_command(
        self, request: TRequest, execution: ParleyExecution[TProgress]
    ) -> ParleyOutcome[TResult, TError]:
        """Execute one admitted command; return a :class:`~parley.errors.ParleyOutcome` value.

        Expected failures are values, not exceptions. Emit progress through
        ``execution.report_progress`` and observe ``execution.signal`` (aborted by a wire ``cnl``,
        H7). A thrown handler, an unserializable result, or a ``$``-reserved custom error aborts the
        command (slot freed, timers cancelled, id tombstoned) — never a leak; the commander then ends
        via its own timers.
        """

    def _on_connected(self) -> None:
        if self._disposed:
            return
        self._subscribe_all()

    def _subscribe_all(self) -> None:
        self._adapter.subscribe(self._topics.request, lambda _t, p: self._on_request_topic(p))

    def _on_request_topic(self, payload: bytes) -> None:
        env = parse(payload)
        if env is None or env.v != PROTOCOL_VERSION or not is_valid_id(env.id):
            return
        if self._disposed:
            return

        if env.t == MessageType.REQUEST:
            self._on_request(env.id, env.p)
        elif env.t == MessageType.RESULT_ACK:
            self._on_result_ack(env.id)
        elif env.t == MessageType.CANCEL:
            self._on_cancel(env.id)
        # else: unknown type, or a handler→commander type on the request topic — ignore (§4).

    def _on_request(self, command_id: str, raw_payload: object | None) -> None:
        # H1: dedup FIRST — the capacity check applies only to unknown ids.
        existing = self._commands.get(command_id)
        if existing is not None:
            if existing.state in (_QUEUED, _EXECUTING):
                self._publish_heartbeat(
                    existing
                )  # immediate hb resynchronizes the commander cheaply
            else:
                self._republish_cached_result(existing)
            return

        # A duplicate of a tombstoned id is ignored (H1).
        if self._tombstones.has(command_id):
            return

        # H2/H8: unknown id — admit if a command slot is free, otherwise stateless $busy.
        if len(self._commands) >= self._config.max_active_commands:
            self._publish_stateless_error(command_id, ParleyBuiltinError.BUSY)
            return

        try:
            request = self._decode_request(raw_payload)
        except Exception:
            # §4 robustness: a req whose payload cannot be decoded is silently ignored — no admission.
            return

        abort = ParleyCancelSignal()
        execution: ParleyExecution[TProgress] = ParleyExecution(
            command_id, abort, lambda progress: self._emit_progress(command_id, progress)
        )
        cmd = _HandlerCommand(command_id, request, abort, execution)
        # Mutate all state BEFORE publishing (re-entrancy safety); the first hb is the accept-ack (H2).
        self._queue.append(cmd)
        self._commands[command_id] = cmd
        self._publish_heartbeat(cmd)
        self._try_dispatch()

    def _on_result_ack(self, command_id: str) -> None:
        # H6: a rak for an id not in RESULT-CACHED is ignored.
        cmd = self._commands.get(command_id)
        if cmd is not None and cmd.state == _RESULT_CACHED:
            self._move_to_tombstone(cmd)

    def _on_cancel(self, command_id: str) -> None:
        cmd = self._commands.get(command_id)
        if cmd is not None:
            if cmd.state == _EXECUTING:
                # H7: abort the execution signal AND cancel the handler task; the eventual outcome is
                # whatever the handler produces (an aborted handler yields $cancelled).
                cmd.abort.abort()
                if cmd.task is not None and not cmd.task.done():
                    cmd.task.cancel()
            elif cmd.state == _QUEUED:
                # H7: dequeue and produce a cached, retried $cancelled result.
                self._remove_from_queue(cmd)
                self._complete_with_result(
                    cmd, self._builtin_error_bytes(command_id, ParleyBuiltinError.CANCELLED), False
                )
            else:  # result-cached
                # H7: cancel lost the race — re-publish the cached result.
                self._republish_cached_result(cmd)
            return

        # H7: unknown or tombstoned id — stateless $cancelled (no state, no rak expected).
        self._publish_stateless_error(command_id, ParleyBuiltinError.CANCELLED)

    def _try_dispatch(self) -> None:
        if self._dispatching:
            return
        self._dispatching = True
        try:
            # H4: dequeue FIFO into at most max_concurrent_executions slots.
            while self._executing_count < self._config.max_concurrent_executions and self._queue:
                cmd = self._queue.popleft()
                cmd.state = _EXECUTING
                self._executing_count += 1
                task: asyncio.Task[None] = asyncio.ensure_future(self._run_command(cmd))
                cmd.task = task
                self._tasks.add(task)
                task.add_done_callback(self._on_task_done)
        finally:
            self._dispatching = False

    def _on_task_done(self, task: asyncio.Task[None]) -> None:
        # Release the strong reference (asyncio holds only weak refs) and retrieve any exception so no
        # "exception was never retrieved" warning is emitted. _run_command contains all faults, so a
        # finished (non-cancelled) task carries no exception during normal operation.
        self._tasks.discard(task)
        if task.cancelled():
            return
        task.exception()

    async def _run_command(self, cmd: _HandlerCommand[TRequest, TProgress]) -> None:
        try:
            outcome = await self.handle_command(cmd.request, cmd.execution)
        except asyncio.CancelledError:
            if self._disposed:
                # close() is awaiting this task — let it unwind so the await retrieves the cancellation.
                raise
            if cmd.abort.aborted:
                # Library-initiated cancellation (a wire cnl): $cancelled via normal completion,
                # cached and retried per H5 (matches C#'s OperationCanceledException arm).
                self._complete_with_result(
                    cmd, self._builtin_error_bytes(cmd.id, ParleyBuiltinError.CANCELLED), True
                )
                return
            # External cancellation the library did not cause — re-raise (asyncio semantics).
            raise
        except Exception:
            if self._disposed:
                return
            # A thrown handler aborts the command so the slot is freed and state stays bounded. No
            # result is produced; the commander ends via its own timers.
            self._abort_executing(cmd)
            return

        if self._disposed:
            return

        # Build the outcome bytes BEFORE mutating any state. A $-reserved/non-string custom error (or
        # an unserializable result) raises; the settled contract treats that exactly like a thrown
        # handler — abort so the slot is freed, timers cancelled, and the id tombstoned.
        try:
            result_bytes = self._build_outcome_bytes(cmd.id, outcome)
        except Exception:
            self._abort_executing(cmd)
            return

        self._complete_with_result(cmd, result_bytes, True)

    def _complete_with_result(
        self, cmd: _HandlerCommand[TRequest, TProgress], result_bytes: bytes, was_executing: bool
    ) -> None:
        # H3: heartbeats run until the command's first res is published.
        if cmd.heartbeat_timer is not None:
            cmd.heartbeat_timer.cancel()
            cmd.heartbeat_timer = None

        cmd.cached_result_bytes = result_bytes
        cmd.state = _RESULT_CACHED

        # H5: retry the result until rak or result_retry_timeout (from this first publication).
        # "Publish comes last": schedule BOTH result timers BEFORE the res publish, so a
        # synchronous-delivery adapter that delivers the rak from inside this publish (moving the
        # command to a tombstone) finds real timers to cancel — never a self-rearming res-storm.
        cmd.result_retry_timer = self._runtime.schedule(
            self._config.result_retry_interval, lambda: self._on_result_retry(cmd)
        )
        cmd.result_retry_timeout_timer = self._runtime.schedule(
            self._config.result_retry_timeout, lambda: self._move_to_tombstone(cmd)
        )

        self._publish(self._topics.result, result_bytes)

        if was_executing:
            self._executing_count -= 1
            self._try_dispatch()

    def _abort_executing(self, cmd: _HandlerCommand[TRequest, TProgress]) -> None:
        if cmd.heartbeat_timer is not None:
            cmd.heartbeat_timer.cancel()
            cmd.heartbeat_timer = None
        self._executing_count -= 1
        self._commands.pop(cmd.id, None)
        self._tombstones.add(cmd.id, self._config.tombstone_ttl)
        self._try_dispatch()

    def _on_result_retry(self, cmd: _HandlerCommand[TRequest, TProgress]) -> None:
        # Reschedule before publishing so a throwing adapter never kills the cadence (self-heals).
        if cmd.result_retry_timer is not None:
            cmd.result_retry_timer.reschedule(self._config.result_retry_interval)
        if cmd.cached_result_bytes is not None:
            self._publish(self._topics.result, cmd.cached_result_bytes)

    def _move_to_tombstone(self, cmd: _HandlerCommand[TRequest, TProgress]) -> None:
        for timer in (cmd.result_retry_timer, cmd.result_retry_timeout_timer, cmd.heartbeat_timer):
            if timer is not None:
                timer.cancel()
        cmd.result_retry_timer = None
        cmd.result_retry_timeout_timer = None
        cmd.heartbeat_timer = None

        # H8: the admission slot is freed at the tombstone transition.
        self._commands.pop(cmd.id, None)
        self._tombstones.add(cmd.id, self._config.tombstone_ttl)

    def _publish_heartbeat(self, cmd: _HandlerCommand[TRequest, TProgress]) -> None:
        # The hb envelope is constant per id — serialize it once and reuse the cached bytes.
        if cmd.cached_heartbeat_bytes is None:
            cmd.cached_heartbeat_bytes = serialize(MessageType.HEARTBEAT, cmd.id)

        # H3: one cadence per id, restarted by every hb publication. (Re)schedule BEFORE publishing so
        # a throwing adapter never kills the cadence.
        if cmd.heartbeat_timer is None:
            cmd.heartbeat_timer = self._runtime.schedule(
                self._config.heartbeat_interval, lambda: self._publish_heartbeat(cmd)
            )
        else:
            cmd.heartbeat_timer.reschedule(self._config.heartbeat_interval)

        self._publish(self._topics.heartbeat, cmd.cached_heartbeat_bytes)

    def _republish_cached_result(self, cmd: _HandlerCommand[TRequest, TProgress]) -> None:
        if cmd.cached_result_bytes is not None:
            self._publish(self._topics.result, cmd.cached_result_bytes)

    def _publish_stateless_error(self, command_id: str, code: ParleyBuiltinError) -> None:
        self._publish(self._topics.result, self._builtin_error_bytes(command_id, code))

    def _emit_progress(self, command_id: str, progress: TProgress) -> None:
        if self._disposed:
            return
        payload = to_payload(self._encode_progress(progress))
        self._publish(self._topics.progress, serialize(MessageType.PROGRESS, command_id, p=payload))

    def _build_outcome_bytes(
        self, command_id: str, outcome: ParleyOutcome[TResult, TError]
    ) -> bytes:
        if outcome.kind == ParleyOutcomeKind.SUCCESS:
            return serialize(
                MessageType.RESULT,
                command_id,
                ok=True,
                p=to_payload(self._encode_result(outcome.value)),  # type: ignore[arg-type]
            )
        if outcome.kind == ParleyOutcomeKind.ERROR:
            code = self._validate_custom_code(outcome.custom_error)
            if outcome.message is None:
                return serialize(MessageType.RESULT, command_id, ok=False, e=code)
            return serialize(MessageType.RESULT, command_id, ok=False, e=code, m=outcome.message)
        # cancelled
        return self._builtin_error_bytes(command_id, ParleyBuiltinError.CANCELLED)

    @staticmethod
    def _validate_custom_code(error: object) -> str:
        # $-prefix fail-fast (§5): custom codes must be plain strings not beginning with '$'.
        if not isinstance(error, str):
            raise ValueError("A custom Parley error must serialize to a plain string code.")
        if error.startswith("$"):
            raise ValueError(
                f"Custom error codes must not begin with '$' (reserved for the protocol): '{error}'."
            )
        return error

    @staticmethod
    def _builtin_error_bytes(command_id: str, code: ParleyBuiltinError) -> bytes:
        return serialize(MessageType.RESULT, command_id, ok=False, e=code.value)

    def _decode_request(self, raw_payload: object | None) -> TRequest:
        if self._codec is not None and self._codec.decode_request is not None:
            return self._codec.decode_request(raw_payload)
        return raw_payload  # type: ignore[return-value]

    def _encode_result(self, value: TResult) -> object:
        if self._codec is not None and self._codec.encode_result is not None:
            return self._codec.encode_result(value)
        return value

    def _encode_progress(self, value: TProgress) -> object:
        if self._codec is not None and self._codec.encode_progress is not None:
            return self._codec.encode_progress(value)
        return value

    def _remove_from_queue(self, cmd: _HandlerCommand[TRequest, TProgress]) -> None:
        with contextlib.suppress(ValueError):
            self._queue.remove(cmd)

    def _publish(self, topic: str, payload: bytes) -> None:
        # Containment: adapter faults must never escape onto library paths (self-healing retries).
        with contextlib.suppress(Exception):
            self._adapter.publish(topic, payload)

    async def close(self) -> None:
        """Close the handler (design.md): detach the connected callback, unsubscribe the request
        topic, cancel every per-command timer, abort every command's cancellation signal, cancel and
        **await** every execution task (retrieving exceptions), and clear all state. Idempotent. After
        close, inbound transport callbacks are ignored and in-flight handlers that later resolve are
        dropped (no publish).
        """
        if self._disposed:
            return
        self._disposed = True

        self._adapter.off_connected(self._on_connected)
        self._adapter.unsubscribe(self._topics.request)

        tasks: list[asyncio.Task[None]] = []
        for cmd in list(self._commands.values()):
            for timer in (
                cmd.heartbeat_timer,
                cmd.result_retry_timer,
                cmd.result_retry_timeout_timer,
            ):
                if timer is not None:
                    timer.cancel()
            cmd.abort.abort()
            if cmd.task is not None and not cmd.task.done():
                cmd.task.cancel()
                tasks.append(cmd.task)

        self._commands.clear()
        self._queue.clear()
        self._executing_count = 0
        self._tombstones.clear()

        if tasks:
            # Await the cancelled tasks so their CancelledError/exception is retrieved (no warnings).
            await asyncio.gather(*tasks, return_exceptions=True)

    # --- internal inspection hooks for the bounded-state tests ---
    @property
    def _command_count(self) -> int:
        return len(self._commands)

    @property
    def _queue_count(self) -> int:
        return len(self._queue)

    @property
    def _executing(self) -> int:
        return self._executing_count

    @property
    def _tombstone_count(self) -> int:
        return self._tombstones.count

    @property
    def _live_timer_count(self) -> int:
        return self._runtime.live_timer_count

    @property
    def _task_count(self) -> int:
        return len(self._tasks)
