"""The conformance vector runner (protocol.md §10), mirroring the TypeScript ``vectorRunner``.

Drives the protocol core through the :class:`FakeTransport` and :class:`VirtualClock` for both roles,
with exact-set ``expect`` matching, step validation, outbox quiescence, outcome coverage, scripted
ids, raw-payload injection, and the fixed-N drain after every step directive. Vector times and config
are in milliseconds; the runner converts to the core's seconds unit at the boundary.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any

from parley import (
    ParleyCallOptions,
    ParleyCancelSignal,
    ParleyCommander,
    ParleyConfig,
    ParleyResult,
    ParleyTopics,
)

from .drain import drain
from .fake_transport import FakeTransport, PublishAction, SubscribeAction
from .scripted_id_generator import ScriptedIdGenerator
from .vector_handler import VectorHandler
from .virtual_clock import VirtualClock

# The topic labels the vectors use double as the actual (opaque) topic strings.
REQUEST = "request"
HEARTBEAT = "heartbeat"
PROGRESS = "progress"
RESULT = "result"

_DIRECTIVE_KEYS = ("call", "cancel", "in", "handler", "connected", "expect")

# Vector config keys (camelCase, milliseconds/counts) → ParleyConfig field (snake_case).
_DURATION_KEYS = {
    "requestRetryInterval": "request_retry_interval",
    "noAckTimeout": "no_ack_timeout",
    "staleTimeout": "stale_timeout",
    "executionTimeout": "execution_timeout",
    "cancelRetryInterval": "cancel_retry_interval",
    "heartbeatInterval": "heartbeat_interval",
    "resultRetryInterval": "result_retry_interval",
    "resultRetryTimeout": "result_retry_timeout",
    "tombstoneTtl": "tombstone_ttl",
}
_COUNT_KEYS = {
    "maxConcurrentExecutions": "max_concurrent_executions",
    "maxActiveCommands": "max_active_commands",
    "maxTombstones": "max_tombstones",
}
_CALL_OPTION_DURATION_KEYS = {
    "requestRetryInterval": "request_retry_interval",
    "noAckTimeout": "no_ack_timeout",
    "staleTimeout": "stale_timeout",
    "executionTimeout": "execution_timeout",
    "cancelRetryInterval": "cancel_retry_interval",
    "tombstoneTtl": "tombstone_ttl",
}


def _ms_to_s(value: Any) -> float:
    if value is None:
        return float("inf")  # a null duration disables (only meaningful for executionTimeout)
    return float(value) / 1000.0


def _config_from_vector(config: dict[str, Any]) -> ParleyConfig:
    kwargs: dict[str, Any] = {}
    for wire_key, field in _DURATION_KEYS.items():
        if wire_key in config:
            kwargs[field] = _ms_to_s(config[wire_key])
    for wire_key, field in _COUNT_KEYS.items():
        if wire_key in config:
            kwargs[field] = int(config[wire_key])
    return ParleyConfig(**kwargs)


def _call_options_from_vector(options: dict[str, Any] | None) -> ParleyCallOptions | None:
    if not options:
        return None
    kwargs: dict[str, Any] = {}
    for wire_key, field in _CALL_OPTION_DURATION_KEYS.items():
        if wire_key in options:
            kwargs[field] = _ms_to_s(options[wire_key])
    return ParleyCallOptions(**kwargs)


async def run_vector(path: str) -> None:
    """Replay one conformance vector file over the fake transport and virtual clock."""
    vector = json.loads(Path(path).read_text(encoding="utf-8"))
    name = vector.get("name", Path(path).name)
    config = _config_from_vector(vector.get("config", {}))
    clock = VirtualClock()
    transport = FakeTransport()
    topics = ParleyTopics(REQUEST, HEARTBEAT, PROGRESS, RESULT)
    role = vector["role"]

    if role == "commander":
        await _run_commander(
            name, config, vector["steps"], vector.get("outcomes"), clock, transport, topics
        )
    elif role == "handler":
        await _run_handler(name, config, vector["steps"], clock, transport, topics)
    else:
        raise AssertionError(f"Vector '{name}': unknown role '{role}'.")


async def _run_commander(
    name: str,
    config: ParleyConfig,
    steps: list[dict[str, Any]],
    outcomes: dict[str, Any] | None,
    clock: VirtualClock,
    transport: FakeTransport,
    topics: ParleyTopics,
) -> None:
    id_generator = ScriptedIdGenerator()
    commander: ParleyCommander[Any, Any, Any, str] = ParleyCommander(
        transport, topics, config, id_generator=id_generator, clock=clock
    )
    transport.clear_outbound()  # construction-time subscribes are not asserted (§10)

    tasks: dict[str, asyncio.Task[ParleyResult[Any, str]]] = {}
    signals: dict[str, ParleyCancelSignal] = {}
    last_at = 0.0

    for step in steps:
        _enforce_single_directive(name, step)
        _assert_valid_at(name, step)
        _assert_non_decreasing(name, step["at"], last_at)
        last_at = step["at"]

        clock.advance_to_ms(step["at"])
        await drain()

        if "call" in step:
            call = step["call"]
            command_id = call["id"]
            id_generator.enqueue(command_id)
            signal = ParleyCancelSignal()
            signals[command_id] = signal
            options = _call_options_from_vector(call.get("options"))
            tasks[command_id] = asyncio.ensure_future(
                commander.execute(call.get("p"), options=options, signal=signal)
            )
        elif "cancel" in step:
            target = step["cancel"]
            if target not in signals:
                raise AssertionError(f"Vector '{name}': cancel for unknown call '{target}'.")
            signals[target].abort()
        elif "in" in step:
            _deliver_inbound(transport, step["in"])
        elif "connected" in step:
            transport.fire_connected()
        elif "expect" in step:
            _assert_expect(name, step["at"], transport.outbound, step["expect"])
            transport.clear_outbound()
        else:
            raise AssertionError(f"Vector '{name}': unrecognized step {step}.")

        await drain()

    _assert_outbox_empty(name, transport)
    await drain()
    _assert_outcomes(name, outcomes, tasks)


async def _run_handler(
    name: str,
    config: ParleyConfig,
    steps: list[dict[str, Any]],
    clock: VirtualClock,
    transport: FakeTransport,
    topics: ParleyTopics,
) -> None:
    handler = VectorHandler(transport, topics, config, clock)
    transport.clear_outbound()  # construction-time subscribes are not asserted (§10)

    last_at = 0.0
    try:
        for step in steps:
            _enforce_single_directive(name, step)
            _assert_valid_at(name, step)
            _assert_non_decreasing(name, step["at"], last_at)
            last_at = step["at"]

            clock.advance_to_ms(step["at"])
            await drain()

            if "in" in step:
                _deliver_inbound(transport, step["in"])
            elif "handler" in step:
                _apply_handler_action(name, handler, step["handler"])
            elif "connected" in step:
                transport.fire_connected()
            elif "expect" in step:
                _assert_expect(name, step["at"], transport.outbound, step["expect"])
                transport.clear_outbound()
            else:
                raise AssertionError(f"Vector '{name}': unrecognized step {step}.")

            await drain()

        _assert_outbox_empty(name, transport)
    finally:
        # Clean up any still-executing scripted handler so no task is left parked on the closing loop.
        # Resolving an executing command can dispatch a queued one (a fresh parked task), so loop until
        # the handler fully quiesces (bounded to avoid a runaway).
        for _ in range(10_000):
            if not handler.has_pending():
                break
            handler.abandon_pending()
            await drain()
        await drain()


def _enforce_single_directive(name: str, step: dict[str, Any]) -> None:
    count = sum(1 for key in _DIRECTIVE_KEYS if key in step)
    if count != 1:
        raise AssertionError(
            f"Vector '{name}': each step must carry exactly one directive, found {count}: {step}"
        )


def _assert_valid_at(name: str, step: dict[str, Any]) -> None:
    at = step.get("at")
    if (
        not isinstance(at, (int, float))
        or isinstance(at, bool)
        or at != at
        or at in (float("inf"), float("-inf"))
    ):
        raise AssertionError(f"Vector '{name}': step 'at' must be a finite number: {step}")


def _assert_non_decreasing(name: str, at: float, last_at: float) -> None:
    if at < last_at:
        raise AssertionError(
            f"Vector '{name}': step times must be non-decreasing ({at} < {last_at})."
        )


def _deliver_inbound(transport: FakeTransport, inbound: dict[str, Any]) -> None:
    if "raw" in inbound:
        payload = inbound["raw"].encode("utf-8")
    else:
        payload = json.dumps(inbound["msg"]).encode("utf-8")
    transport.deliver(inbound["topic"], payload)


def _apply_handler_action(name: str, handler: VectorHandler, action: dict[str, Any]) -> None:
    kind = action["action"]
    if kind == "complete":
        handler.complete(action["id"], action.get("p"))
    elif kind == "fail":
        handler.fail(action["id"], action["e"], action.get("m"))
    elif kind == "progress":
        handler.emit_progress(action["id"], action.get("p"))
    else:
        raise AssertionError(f"Vector '{name}': unknown handler action '{kind}'.")


def _assert_expect(
    name: str, at: float, actual: list[object], expected: list[dict[str, Any]]
) -> None:
    unmatched = list(actual)

    for item in expected:
        index = -1
        if "subscribe" in item:
            for i, action in enumerate(unmatched):
                if isinstance(action, SubscribeAction) and action.topic == item["subscribe"]:
                    index = i
                    break
        else:
            for i, action in enumerate(unmatched):
                if (
                    isinstance(action, PublishAction)
                    and action.topic == item["topic"]
                    and json.loads(action.payload.decode("utf-8")) == item["msg"]
                ):
                    index = i
                    break

        if index < 0:
            raise AssertionError(
                f"Vector '{name}' at t={at}: expected outbound action not found: {item}\n"
                f"Actual: {_dump(actual)}"
            )
        unmatched.pop(index)

    if unmatched:
        raise AssertionError(
            f"Vector '{name}' at t={at}: unexpected extra outbound action(s): {_dump(unmatched)}\n"
            f"Expected: {expected}"
        )


def _assert_outbox_empty(name: str, transport: FakeTransport) -> None:
    if transport.outbound:
        raise AssertionError(
            f"Vector '{name}': {len(transport.outbound)} outbound action(s) left unasserted after the "
            f"final step: {_dump(transport.outbound)}"
        )


def _assert_outcomes(
    name: str,
    outcomes: dict[str, Any] | None,
    tasks: dict[str, asyncio.Task[ParleyResult[Any, str]]],
) -> None:
    # Every started call must have a declared expected outcome — nothing silently unasserted.
    for started_id in tasks:
        if not outcomes or started_id not in outcomes:
            raise AssertionError(
                f"Vector '{name}': call '{started_id}' was started but has no expected outcome declared."
            )

    if not outcomes:
        return

    for command_id, expected in outcomes.items():
        task = tasks.get(command_id)
        if task is None:
            raise AssertionError(
                f"Vector '{name}': outcome expected for call '{command_id}' that was never started."
            )
        if not task.done():
            raise AssertionError(
                f"Vector '{name}': call '{command_id}' did not reach a terminal outcome."
            )
        if task.cancelled():
            raise AssertionError(f"Vector '{name}': call '{command_id}' task was cancelled.")
        exc = task.exception()
        if exc is not None:
            raise AssertionError(
                f"Vector '{name}': call '{command_id}' rejected unexpectedly: {exc!r}"
            )

        result = task.result()
        if expected["ok"]:
            if not result.is_success:
                assert result.error is not None
                raise AssertionError(
                    f"Vector '{name}': call '{command_id}' expected success but got error "
                    f"'{result.error.code}'."
                )
            if "p" not in expected:
                if result.value is not None:
                    raise AssertionError(
                        f"Vector '{name}': call '{command_id}' expected absent/null payload but got "
                        f"{result.value!r}."
                    )
            elif result.value != expected["p"]:
                raise AssertionError(
                    f"Vector '{name}': call '{command_id}' payload mismatch. Expected {expected['p']!r}, "
                    f"got {result.value!r}."
                )
        else:
            if result.is_success:
                raise AssertionError(
                    f"Vector '{name}': call '{command_id}' expected error but succeeded."
                )
            assert result.error is not None
            if result.error.code != expected["e"]:
                raise AssertionError(
                    f"Vector '{name}': call '{command_id}' error code mismatch. Expected "
                    f"'{expected['e']}', got '{result.error.code}'."
                )


def _dump(actions: list[object]) -> str:
    parts = []
    for action in actions:
        if isinstance(action, SubscribeAction):
            parts.append(f"{{subscribe: {action.topic}}}")
        elif isinstance(action, PublishAction):
            parts.append(f"{{topic: {action.topic}, msg: {action.payload.decode('utf-8')}}}")
    return "[" + ", ".join(parts) + "]"
