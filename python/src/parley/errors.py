"""Error codes, results, and outcomes (protocol.md §5, design.md).

Expected outcomes — including all six builtin codes — are returned as :class:`ParleyResult` values,
never raised (``.claude/CONVENTIONS.md``: "expected outcomes return as values"). Exceptions are
reserved for programming errors (bad config, misuse).
"""

from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Final, Generic, TypeVar

TResult = TypeVar("TResult")
TError = TypeVar("TError")


class ParleyBuiltinError(str, Enum):
    """The six builtin protocol error codes (protocol.md §5).

    Each member is its ``$``-prefixed wire string; because the enum subclasses :class:`str`, a member
    compares equal to and serializes as that string.
    """

    NO_ACK = "$noAck"
    """The sign-of-life window expired before any sign of life (handler unreachable)."""
    EXECUTION_STALE = "$executionStale"
    """The sign-of-life window expired after signs of life (handler went silent)."""
    EXECUTION_TIMEOUT = "$executionTimeout"
    """The overall execution cap expired (handler alive but never finished)."""
    CANCELLED = "$cancelled"
    """The command was cancelled before producing a result."""
    BUSY = "$busy"
    """The handler refused admission because its command pool is full."""
    DECODE_ERROR = "$decodeError"
    """A success ``res`` arrived but its payload did not decode (commander-local; never on the wire)."""


_BUILTIN_BY_VALUE: Final = {member.value: member for member in ParleyBuiltinError}


def to_builtin(code: str) -> ParleyBuiltinError | None:
    """Map a wire error string onto a builtin code, or ``None`` when it is a custom code."""
    return _BUILTIN_BY_VALUE.get(code)


@dataclass(frozen=True, slots=True)
class ParleyError(Generic[TError]):
    """The error arm of a :class:`ParleyResult`.

    ``code`` is the raw wire string; ``builtin`` is set when ``code`` is one of the six builtin
    codes; ``custom`` carries the consumer's typed error otherwise; ``message`` is the optional ``m``
    detail (protocol.md §4.1/§5).
    """

    code: str
    builtin: ParleyBuiltinError | None = None
    custom: TError | None = None
    message: str | None = None

    @property
    def is_builtin(self) -> bool:
        """Whether ``code`` is one of the six builtin protocol codes."""
        return self.builtin is not None


@dataclass(frozen=True, slots=True)
class ParleyResult(Generic[TResult, TError]):
    """The single terminal outcome a commander observes for a command (protocol.md §5).

    ``is_success`` discriminates: on success, ``value`` carries the (possibly ``None``) result
    payload and ``error`` is ``None``; on failure, ``error`` carries the :class:`ParleyError` and
    ``value`` is ``None``. Every expected outcome is a value, never a raised exception.
    """

    is_success: bool
    value: TResult | None = None
    error: ParleyError[TError] | None = None

    @classmethod
    def success(cls, value: TResult | None) -> ParleyResult[TResult, TError]:
        """A successful result carrying a (possibly absent) payload."""
        return cls(is_success=True, value=value)

    @classmethod
    def failure(cls, error: ParleyError[TError]) -> ParleyResult[TResult, TError]:
        """An error result."""
        return cls(is_success=False, error=error)


class ParleyOutcomeKind(str, Enum):
    """The kind tag for a :class:`ParleyOutcome`."""

    SUCCESS = "success"
    ERROR = "error"
    CANCELLED = "cancelled"


@dataclass(frozen=True, slots=True)
class ParleyOutcome(Generic[TResult, TError]):
    """The outcome a handler returns for one command.

    A success payload, a custom error, or cancellation (emitted on the wire as ``$cancelled``).
    Construct via :meth:`success`, :meth:`error`, or :meth:`cancelled`.
    """

    kind: ParleyOutcomeKind
    value: TResult | None = None
    custom_error: TError | None = None
    message: str | None = None

    @classmethod
    def success(cls, value: TResult | None) -> ParleyOutcome[TResult, TError]:
        """A successful outcome carrying a result payload."""
        return cls(kind=ParleyOutcomeKind.SUCCESS, value=value)

    @classmethod
    def error(cls, error: TError, message: str | None = None) -> ParleyOutcome[TResult, TError]:
        """A custom-error outcome. The code must be a plain string not beginning with ``$`` (§5)."""
        return cls(kind=ParleyOutcomeKind.ERROR, custom_error=error, message=message)

    @classmethod
    def cancelled(cls) -> ParleyOutcome[TResult, TError]:
        """A cancelled outcome, emitted on the wire as ``$cancelled``."""
        return cls(kind=ParleyOutcomeKind.CANCELLED)
