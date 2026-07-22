"""Topics, timing configuration, and per-call options (protocol.md §3/§8, design.md).

Durations are **seconds** (float) — the Python-idiomatic unit (design.md's ``execution_timeout=600.0``
is ten minutes). ``execution_timeout`` is tri-state: on :class:`ParleyConfig` a positive number caps
execution and ``math.inf`` disables the cap; on :class:`ParleyCallOptions` ``None`` inherits the
config value, ``math.inf`` disables the cap for this call, and a positive number overrides it.
"""

from __future__ import annotations

import math
from dataclasses import dataclass


# The validators take `object` (not the declared field type) so their isinstance guards stay
# meaningful against untyped callers — a str where a float is declared must raise, not slip through.
def _require_positive(value: object, name: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f"Parley config '{name}' must be a number of seconds.")
    if not math.isfinite(value) or value <= 0:
        raise ValueError(
            f"Parley config '{name}' must be a strictly positive, finite number of seconds."
        )
    return float(value)


def _require_positive_or_inf(value: object, name: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(
            f"Parley config '{name}' must be a number of seconds (or math.inf to disable)."
        )
    if value == math.inf:
        return float(value)
    if not math.isfinite(value) or value <= 0:
        raise ValueError(
            f"Parley config '{name}' must be a strictly positive number of seconds, or math.inf to disable."
        )
    return float(value)


def _require_count(value: object, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 1:
        raise ValueError(f"Parley config '{name}' must be an integer >= 1.")
    return value


def _validate_topic(topic: object, name: str) -> None:
    if not isinstance(topic, str) or len(topic) == 0:
        raise ValueError(f"Parley topic '{name}' must be a non-empty string.")
    if "+" in topic or "#" in topic:
        raise ValueError(f"Parley topic '{name}' must not contain MQTT wildcards ('+' or '#').")


@dataclass(frozen=True, slots=True)
class ParleyTopics:
    """The four consumer-supplied topic strings for one command type (protocol.md §3).

    Opaque, distinct within the set, no MQTT wildcards.
    """

    request: str
    heartbeat: str
    progress: str
    result: str

    def __post_init__(self) -> None:
        _validate_topic(self.request, "request")
        _validate_topic(self.heartbeat, "heartbeat")
        _validate_topic(self.progress, "progress")
        _validate_topic(self.result, "result")
        if len({self.request, self.heartbeat, self.progress, self.result}) != 4:
            raise ValueError("Parley topics must be distinct within the set.")


@dataclass(frozen=True, slots=True)
class ParleyConfig:
    """Per-deployment timing configuration and handler capacity caps (protocol.md §8).

    All fields have the §8 defaults. ``execution_timeout`` is a positive number of seconds or
    ``math.inf`` (no cap).
    """

    request_retry_interval: float = 2.0
    """C1 — ``req`` re-publish interval while SENDING."""
    no_ack_timeout: float = 10.0
    """C3 — sign-of-life window before any sign of life (SENDING)."""
    stale_timeout: float = 15.0
    """C3 — sliding sign-of-life window after signs of life (LIVE)."""
    execution_timeout: float = 60.0
    """C2/C3 — overall execution cap from the first sign of life; ``math.inf`` disables it."""
    cancel_retry_interval: float = 2.0
    """C7 — ``cnl`` re-publish interval while cancelling."""
    heartbeat_interval: float = 3.0
    """H3 — ``hb`` interval per admitted command."""
    result_retry_interval: float = 2.0
    """H5 — ``res`` re-publish interval until acked."""
    result_retry_timeout: float = 30.0
    """H5 — result-retry give-up window, from the first ``res`` publication."""
    tombstone_ttl: float = 60.0
    """C5/H5 — tombstone retention for duplicate suppression."""
    max_concurrent_executions: int = 1
    """H4 — maximum commands executing concurrently (default 1, serial)."""
    max_active_commands: int = 32
    """H2/H8 — admission cap; a slot is held from admission through RESULT-CACHED."""
    max_tombstones: int = 4096
    """Hard cap on the tombstone table (oldest-first eviction)."""

    def __post_init__(self) -> None:
        _require_positive(self.request_retry_interval, "request_retry_interval")
        _require_positive(self.no_ack_timeout, "no_ack_timeout")
        _require_positive(self.stale_timeout, "stale_timeout")
        _require_positive_or_inf(self.execution_timeout, "execution_timeout")
        _require_positive(self.cancel_retry_interval, "cancel_retry_interval")
        _require_positive(self.heartbeat_interval, "heartbeat_interval")
        _require_positive(self.result_retry_interval, "result_retry_interval")
        _require_positive(self.result_retry_timeout, "result_retry_timeout")
        _require_positive(self.tombstone_ttl, "tombstone_ttl")
        _require_count(self.max_concurrent_executions, "max_concurrent_executions")
        _require_count(self.max_active_commands, "max_active_commands")
        _require_count(self.max_tombstones, "max_tombstones")


@dataclass(frozen=True, slots=True)
class ParleyCallOptions:
    """Per-call overrides of the commander-local timing parameters (protocol.md §8).

    ``None`` inherits the configured value for every field. For ``execution_timeout`` only,
    ``math.inf`` additionally disables the cap for this call (tri-state).
    """

    request_retry_interval: float | None = None
    no_ack_timeout: float | None = None
    stale_timeout: float | None = None
    execution_timeout: float | None = None
    cancel_retry_interval: float | None = None
    tombstone_ttl: float | None = None

    def __post_init__(self) -> None:
        if self.request_retry_interval is not None:
            _require_positive(self.request_retry_interval, "request_retry_interval")
        if self.no_ack_timeout is not None:
            _require_positive(self.no_ack_timeout, "no_ack_timeout")
        if self.stale_timeout is not None:
            _require_positive(self.stale_timeout, "stale_timeout")
        if self.execution_timeout is not None:
            _require_positive_or_inf(self.execution_timeout, "execution_timeout")
        if self.cancel_retry_interval is not None:
            _require_positive(self.cancel_retry_interval, "cancel_retry_interval")
        if self.tombstone_ttl is not None:
            _require_positive(self.tombstone_ttl, "tombstone_ttl")


@dataclass(frozen=True, slots=True)
class ResolvedCallOptions:
    """Per-call resolved commander timing (all present). ``execution_timeout`` may be ``math.inf``."""

    request_retry_interval: float
    no_ack_timeout: float
    stale_timeout: float
    execution_timeout: float
    cancel_retry_interval: float
    tombstone_ttl: float


def resolve_call_options(
    config: ParleyConfig, options: ParleyCallOptions | None
) -> ResolvedCallOptions:
    """Resolve per-call options against ``config``.

    ``None`` inherits the config value for every field. ``execution_timeout`` uses explicit
    branching (never an ``or``-collapse): ``None`` inherits the config value (which may itself be
    ``math.inf``), ``math.inf`` disables the cap for this call, and a positive number overrides it.
    """
    if options is None:
        options = _EMPTY_OPTIONS

    if options.execution_timeout is None:
        execution_timeout = config.execution_timeout
    else:
        execution_timeout = options.execution_timeout

    return ResolvedCallOptions(
        request_retry_interval=(
            options.request_retry_interval
            if options.request_retry_interval is not None
            else config.request_retry_interval
        ),
        no_ack_timeout=(
            options.no_ack_timeout if options.no_ack_timeout is not None else config.no_ack_timeout
        ),
        stale_timeout=(
            options.stale_timeout if options.stale_timeout is not None else config.stale_timeout
        ),
        execution_timeout=execution_timeout,
        cancel_retry_interval=(
            options.cancel_retry_interval
            if options.cancel_retry_interval is not None
            else config.cancel_retry_interval
        ),
        tombstone_ttl=(
            options.tombstone_ttl if options.tombstone_ttl is not None else config.tombstone_ttl
        ),
    )


_EMPTY_OPTIONS = ParleyCallOptions()
