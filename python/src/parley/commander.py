"""The caller side of one command type (protocol.md §6, rules C1–C8).

Multiple commanders MAY share a topic set; each filters inbound traffic by command id. Expected
outcomes — including all six builtin codes — resolve as :class:`~parley.errors.ParleyResult` values,
never raised.

Architecture (binding, per the approved plan): a synchronous core with a single async island. Inbound
delivery, timer callbacks, and every transition run synchronously; ``execute`` awaits an
:class:`asyncio.Future` that a transition resolves synchronously. "Publish comes last": all state
mutation and timer scheduling for a transition complete before ``adapter.publish``, so a
synchronous-delivery (loopback) adapter that re-enters mid-transition finds consistent state.
"""

from __future__ import annotations

import asyncio
import contextlib
from collections.abc import Callable
from dataclasses import dataclass
from typing import Generic, TypeVar

from ._envelope import (
    PROTOCOL_VERSION,
    MessageType,
    is_valid_id,
    parse,
    serialize,
    to_payload,
)
from ._runtime import ParleyRuntime, ParleyTimer
from ._tombstones import TombstoneTable
from .adapter import ParleyConnectionAdapter
from .clock import LoopClock, ParleyClock
from .config import (
    ParleyCallOptions,
    ParleyConfig,
    ParleyTopics,
    ResolvedCallOptions,
    resolve_call_options,
)
from .errors import ParleyBuiltinError, ParleyError, ParleyResult, to_builtin
from .execution import ParleyCancelSignalProtocol
from .id_generator import DefaultIdGenerator, ParleyIdGenerator

TRequest = TypeVar("TRequest")
TProgress = TypeVar("TProgress")
TResult = TypeVar("TResult")
TError = TypeVar("TError")

_SENDING = "sending"
_LIVE = "live"


@dataclass(frozen=True, slots=True)
class ParleyCommanderCodec(Generic[TRequest, TResult, TProgress]):
    """Optional per-commander codec hooks (design.md). All default to a raw pass-through.

    ``encode_request`` throwing surfaces synchronously out of :meth:`ParleyCommander.execute` (a
    caller-visible programming error; no command state is created). ``decode_result`` throwing yields
    ``$decodeError`` (a ``rak`` is still published, per C4). ``decode_progress`` throwing drops the
    progress update silently (C8).
    """

    encode_request: Callable[[TRequest], object] | None = None
    decode_result: Callable[[object], TResult] | None = None
    decode_progress: Callable[[object], TProgress] | None = None


class _Command(Generic[TProgress, TResult, TError]):
    __slots__ = (
        "cancel_bytes",
        "cancel_retry_timer",
        "cancelling",
        "execution_cap_timer",
        "future",
        "id",
        "on_progress",
        "options",
        "request_bytes",
        "request_retry_timer",
        "sign_of_life_timer",
        "signal",
        "signal_listener",
        "state",
    )

    def __init__(
        self,
        command_id: str,
        future: asyncio.Future[ParleyResult[TResult, TError]],
        on_progress: Callable[[TProgress], None] | None,
        options: ResolvedCallOptions,
        request_bytes: bytes,
    ) -> None:
        self.id = command_id
        self.future = future
        self.on_progress = on_progress
        self.options = options
        self.request_bytes = request_bytes
        self.state = _SENDING
        self.cancelling = False
        self.cancel_bytes: bytes | None = None
        self.request_retry_timer: ParleyTimer | None = None
        self.sign_of_life_timer: ParleyTimer | None = None
        self.execution_cap_timer: ParleyTimer | None = None
        self.cancel_retry_timer: ParleyTimer | None = None
        self.signal: ParleyCancelSignalProtocol | None = None
        self.signal_listener: Callable[[], None] | None = None


class ParleyCommander(Generic[TRequest, TProgress, TResult, TError]):
    """The caller side of one command type (protocol.md §6, rules C1–C8)."""

    __slots__ = (
        "_adapter",
        "_codec",
        "_config",
        "_disposed",
        "_id_generator",
        "_inflight",
        "_runtime",
        "_tombstones",
        "_topics",
    )

    def __init__(
        self,
        adapter: ParleyConnectionAdapter,
        topics: ParleyTopics,
        config: ParleyConfig | None = None,
        codec: ParleyCommanderCodec[TRequest, TResult, TProgress] | None = None,
        *,
        id_generator: ParleyIdGenerator | None = None,
        clock: ParleyClock | None = None,
    ) -> None:
        self._adapter = adapter
        self._topics = topics
        self._config = config if config is not None else ParleyConfig()
        self._runtime = ParleyRuntime(clock if clock is not None else LoopClock())
        self._tombstones = TombstoneTable(self._runtime, self._config.max_tombstones)
        self._id_generator = id_generator if id_generator is not None else DefaultIdGenerator()
        self._codec = codec
        self._inflight: dict[str, _Command[TProgress, TResult, TError]] = {}
        self._disposed = False

        self._adapter.on_connected(self._on_connected)
        self._subscribe_all()

    def _on_connected(self) -> None:
        if self._disposed:
            return
        self._subscribe_all()

    def _subscribe_all(self) -> None:
        self._adapter.subscribe(
            self._topics.heartbeat, lambda _t, p: self._on_inbound(p, MessageType.HEARTBEAT)
        )
        self._adapter.subscribe(
            self._topics.progress, lambda _t, p: self._on_inbound(p, MessageType.PROGRESS)
        )
        self._adapter.subscribe(
            self._topics.result, lambda _t, p: self._on_inbound(p, MessageType.RESULT)
        )

    async def execute(
        self,
        request: TRequest,
        options: ParleyCallOptions | None = None,
        on_progress: Callable[[TProgress], None] | None = None,
        signal: ParleyCancelSignalProtocol | None = None,
    ) -> ParleyResult[TResult, TError]:
        """Execute one command and await its single terminal outcome (protocol.md C1–C5).

        Aborting ``signal`` triggers a wire-level ``cnl`` (C7); the returned result is still whatever
        the command actually reaches (a genuine result may win the race). Never raises for an expected
        outcome. Raises on misuse (closed instance, invalid/colliding id) or when a configured
        ``encode_request`` raises — fail-fast, before any command state is created.
        """
        if self._disposed:
            raise RuntimeError("ParleyCommander is closed.")

        command_id = self._id_generator.new_id()
        if not is_valid_id(command_id):
            raise RuntimeError(f"Command-id generator produced an invalid id: '{command_id}'.")
        if command_id in self._inflight or self._tombstones.has(command_id):
            raise RuntimeError(f"Command id '{command_id}' collides with existing state.")

        # Encode the request up front: a throwing encode_request is a caller-visible programming error
        # — let it propagate before ANY per-command state (inflight, timers, signal listener) exists,
        # so a failed encode leaves nothing to unwind. Consistent with fail-fast config/id validation.
        request_bytes = serialize(
            MessageType.REQUEST, command_id, p=to_payload(self._encode_request(request))
        )

        loop = asyncio.get_running_loop()
        future: asyncio.Future[ParleyResult[TResult, TError]] = loop.create_future()
        cmd: _Command[TProgress, TResult, TError] = _Command(
            command_id,
            future,
            on_progress,
            resolve_call_options(self._config, options),
            request_bytes,
        )
        self._inflight[command_id] = cmd

        # "Publish comes last": schedule the request-retry and sign-of-life timers BEFORE the initial
        # publish, so a synchronous-delivery adapter that re-enters (an hb/res delivered from inside
        # publish) finds real timers to cancel/reschedule.
        cmd.request_retry_timer = self._runtime.schedule(
            cmd.options.request_retry_interval, lambda: self._on_request_retry(cmd)
        )
        cmd.sign_of_life_timer = self._runtime.schedule(
            cmd.options.no_ack_timeout, lambda: self._on_sign_of_life_expiry(cmd)
        )

        self._publish(self._topics.request, cmd.request_bytes)

        # Signal wiring stays AFTER the initial publish so the wire order is req-then-cnl for an
        # already-aborted signal. Guard on still-in-flight: a synchronous reply during the publish
        # above may already have completed and torn the command down (no listener to attach then).
        if signal is not None and command_id in self._inflight:
            if signal.aborted:
                self._request_cancel(command_id)
            else:
                listener = self._make_cancel_listener(command_id)
                signal.add_listener(listener)
                cmd.signal = signal
                cmd.signal_listener = listener

        return await future

    def _make_cancel_listener(self, command_id: str) -> Callable[[], None]:
        def listener() -> None:
            self._request_cancel(command_id)

        return listener

    def _request_cancel(self, command_id: str) -> None:
        cmd = self._inflight.get(command_id)
        if self._disposed or cmd is None or cmd.cancelling:
            return
        cmd.cancelling = True

        # C7: a command the caller no longer wants must not keep being offered for admission.
        if cmd.state == _SENDING and cmd.request_retry_timer is not None:
            cmd.request_retry_timer.cancel()
            cmd.request_retry_timer = None

        cmd.cancel_bytes = serialize(MessageType.CANCEL, command_id)
        # "Publish comes last": schedule the cancel-retry timer BEFORE publishing the cnl. A
        # synchronous-delivery adapter can deliver the $cancelled res from inside this publish and tear
        # the command down; the re-entrant teardown must find the timer to cancel it, otherwise it
        # orphans a self-rearming timer that re-publishes cnl forever (a permanent cnl-storm leak).
        cmd.cancel_retry_timer = self._runtime.schedule(
            cmd.options.cancel_retry_interval, lambda: self._on_cancel_retry(cmd)
        )
        self._publish(self._topics.request, cmd.cancel_bytes)

    def _on_request_retry(self, cmd: _Command[TProgress, TResult, TError]) -> None:
        # Reschedule before publishing so a throwing adapter never kills the cadence (self-heals).
        if cmd.request_retry_timer is not None:
            cmd.request_retry_timer.reschedule(cmd.options.request_retry_interval)
        self._publish(self._topics.request, cmd.request_bytes)

    def _on_cancel_retry(self, cmd: _Command[TProgress, TResult, TError]) -> None:
        if cmd.cancel_retry_timer is not None:
            cmd.cancel_retry_timer.reschedule(cmd.options.cancel_retry_interval)
        if cmd.cancel_bytes is not None:
            self._publish(self._topics.request, cmd.cancel_bytes)

    def _on_sign_of_life_expiry(self, cmd: _Command[TProgress, TResult, TError]) -> None:
        # SENDING → $noAck (never any sign of life); LIVE → $executionStale (went silent).
        self._finish_builtin(
            cmd,
            ParleyBuiltinError.NO_ACK
            if cmd.state == _SENDING
            else ParleyBuiltinError.EXECUTION_STALE,
        )

    def _on_inbound(self, payload: bytes, expected_type: str) -> None:
        env = parse(payload)
        if (
            env is None
            or env.v != PROTOCOL_VERSION
            or env.t != expected_type
            or not is_valid_id(env.id)
        ):
            return
        if self._disposed:
            return

        if expected_type == MessageType.HEARTBEAT:
            self._on_heartbeat(env.id)
        elif expected_type == MessageType.PROGRESS:
            self._on_progress(env.id, env.p)
        elif expected_type == MessageType.RESULT:
            self._on_result(env.id, env.ok, env.e, env.m, env.p)

    def _on_heartbeat(self, command_id: str) -> None:
        cmd = self._inflight.get(command_id)
        if cmd is not None:
            self._on_sign_of_life(cmd)

    def _on_progress(self, command_id: str, raw_payload: object | None) -> None:
        cmd = self._inflight.get(command_id)
        if cmd is None:
            return

        self._on_sign_of_life(cmd)

        sink = cmd.on_progress
        if sink is None:
            return
        try:
            value = self._decode_progress(raw_payload)
        except Exception:
            # C8: progress is advisory — a prg that fails to decode is dropped (still a sign of life).
            return
        # Containment: a consumer progress callback must never escape onto library paths.
        with contextlib.suppress(Exception):
            sink(value)

    def _on_result(
        self,
        command_id: str,
        ok: bool | None,
        e: str | None,
        m: str | None,
        raw_payload: object | None,
    ) -> None:
        # ok is required on res; e is required when ok:false (§4).
        if ok is None:
            return
        if ok is False and e is None:
            return

        is_busy = ok is False and e == ParleyBuiltinError.BUSY.value
        cmd = self._inflight.get(command_id)

        if cmd is not None:
            # C4 exception: a $busy res in LIVE can only be a stale pre-admission reply — ignore it
            # (and it is NOT a sign of life, so the sliding window is not reset).
            if cmd.state == _LIVE and is_busy:
                return

            if ok:
                # C4: decode the payload BEFORE acking. A success payload that fails to decode is a peer
                # contract mismatch — still publish rak (redelivery cannot fix it) and terminate
                # $decodeError.
                decoded = True
                value: TResult | None = None
                try:
                    value = self._decode_result(raw_payload)
                except Exception:
                    decoded = False

                self._publish_result_ack(command_id)
                if decoded:
                    self._finish(cmd, ParleyResult.success(value))
                else:
                    self._finish_builtin(cmd, ParleyBuiltinError.DECODE_ERROR)
            else:
                # C4: rak every terminal error res except $busy (handler holds no state for a refused id).
                if not is_busy:
                    self._publish_result_ack(command_id)
                # e is guaranteed present here (checked above).
                assert e is not None
                self._finish(cmd, ParleyResult.failure(self._build_error(e, m)))
            return

        # C5: a res for a tombstoned id re-emits rak (unless $busy) and discards the content.
        if self._tombstones.has(command_id):
            if not is_busy:
                self._publish_result_ack(command_id)
            return

        # C6: neither in-flight nor tombstoned — belongs to another commander instance. Ignore.

    def _on_sign_of_life(self, cmd: _Command[TProgress, TResult, TError]) -> None:
        if cmd.state == _SENDING:
            # C2: first sign of life — enter LIVE, stop req retries, start the execution cap.
            cmd.state = _LIVE
            if cmd.request_retry_timer is not None:
                cmd.request_retry_timer.cancel()
                cmd.request_retry_timer = None

            cap = cmd.options.execution_timeout
            # Tri-state: math.inf disables the cap — the disable branch skips scheduling entirely.
            if cap != float("inf"):
                cmd.execution_cap_timer = self._runtime.schedule(
                    cap, lambda: self._finish_builtin(cmd, ParleyBuiltinError.EXECUTION_TIMEOUT)
                )

        # The sliding sign-of-life timer restarts on every sign of life (C2/C3).
        if cmd.sign_of_life_timer is not None:
            cmd.sign_of_life_timer.reschedule(cmd.options.stale_timeout)

    def _encode_request(self, value: TRequest) -> object:
        if self._codec is not None and self._codec.encode_request is not None:
            return self._codec.encode_request(value)
        return value

    def _decode_result(self, raw_payload: object | None) -> TResult:
        if self._codec is not None and self._codec.decode_result is not None:
            return self._codec.decode_result(raw_payload)
        return raw_payload  # type: ignore[return-value]

    def _decode_progress(self, raw_payload: object | None) -> TProgress:
        if self._codec is not None and self._codec.decode_progress is not None:
            return self._codec.decode_progress(raw_payload)
        return raw_payload  # type: ignore[return-value]

    def _build_error(self, code: str, message: str | None) -> ParleyError[TError]:
        builtin = to_builtin(code)
        if builtin is not None:
            return ParleyError(code=code, builtin=builtin, message=message)
        # Custom errors derive from the e string code only (structured error p is not surfaced in v1).
        custom: TError = code  # type: ignore[assignment]
        return ParleyError(code=code, custom=custom, message=message)

    def _finish_builtin(
        self, cmd: _Command[TProgress, TResult, TError], builtin: ParleyBuiltinError
    ) -> None:
        self._finish(cmd, ParleyResult.failure(ParleyError(code=builtin.value, builtin=builtin)))

    def _finish(
        self,
        cmd: _Command[TProgress, TResult, TError],
        result: ParleyResult[TResult, TError],
    ) -> None:
        if cmd.id not in self._inflight:
            return
        self._teardown(cmd)
        if not cmd.future.done():
            cmd.future.set_result(result)

    def _teardown(self, cmd: _Command[TProgress, TResult, TError]) -> None:
        for timer in (
            cmd.request_retry_timer,
            cmd.sign_of_life_timer,
            cmd.execution_cap_timer,
            cmd.cancel_retry_timer,
        ):
            if timer is not None:
                timer.cancel()
        cmd.request_retry_timer = None
        cmd.sign_of_life_timer = None
        cmd.execution_cap_timer = None
        cmd.cancel_retry_timer = None
        if cmd.signal is not None and cmd.signal_listener is not None:
            cmd.signal.remove_listener(cmd.signal_listener)
        cmd.signal = None
        cmd.signal_listener = None

        del self._inflight[cmd.id]
        # C5: every terminal outcome tombstones the id for tombstone_ttl.
        self._tombstones.add(cmd.id, cmd.options.tombstone_ttl)

    def _publish_result_ack(self, command_id: str) -> None:
        self._publish(self._topics.request, serialize(MessageType.RESULT_ACK, command_id))

    def _publish(self, topic: str, payload: bytes) -> None:
        # Containment: adapter faults (publish on a disconnected/disposed client) must never escape
        # onto library paths. Retries reschedule before publishing, so delivery self-heals.
        with contextlib.suppress(Exception):
            self._adapter.publish(topic, payload)

    async def close(self) -> None:
        """Close the commander (design.md): detach the connected callback, unsubscribe the library's
        topics, cancel every per-command timer, resolve every in-flight ``execute`` future locally
        with a ``$cancelled`` *value* (never task-cancellation), and clear all state. Idempotent.
        After close, :meth:`execute` raises and inbound transport callbacks are ignored.
        """
        if self._disposed:
            return
        self._disposed = True

        self._adapter.off_connected(self._on_connected)
        self._adapter.unsubscribe(self._topics.heartbeat)
        self._adapter.unsubscribe(self._topics.progress)
        self._adapter.unsubscribe(self._topics.result)

        cancelled: ParleyResult[TResult, TError] = ParleyResult.failure(
            ParleyError(
                code=ParleyBuiltinError.CANCELLED.value, builtin=ParleyBuiltinError.CANCELLED
            )
        )

        for cmd in list(self._inflight.values()):
            for timer in (
                cmd.request_retry_timer,
                cmd.sign_of_life_timer,
                cmd.execution_cap_timer,
                cmd.cancel_retry_timer,
            ):
                if timer is not None:
                    timer.cancel()
            if cmd.signal is not None and cmd.signal_listener is not None:
                cmd.signal.remove_listener(cmd.signal_listener)
            if not cmd.future.done():
                cmd.future.set_result(cancelled)

        self._inflight.clear()
        self._tombstones.clear()

    # --- internal inspection hooks for the bounded-state tests ---
    @property
    def _inflight_count(self) -> int:
        return len(self._inflight)

    @property
    def _tombstone_count(self) -> int:
        return self._tombstones.count

    @property
    def _live_timer_count(self) -> int:
        return self._runtime.live_timer_count
