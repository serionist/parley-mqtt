"""Parley — request/response RPC over MQTT (Python/asyncio reference implementation of protocol v1).

Bring your own MQTT client via a :class:`ParleyConnectionAdapter`; the core is transport-free and has
zero required runtime dependencies. Expected outcomes resolve as :class:`ParleyResult` values — never
raised exceptions.
"""

from __future__ import annotations

from .adapter import ParleyConnectionAdapter, ParleyMessageHandler
from .clock import LoopClock, ParleyClock, ParleyTimerHandle
from .commander import ParleyCommander, ParleyCommanderCodec
from .config import ParleyCallOptions, ParleyConfig, ParleyTopics
from .errors import (
    ParleyBuiltinError,
    ParleyError,
    ParleyOutcome,
    ParleyOutcomeKind,
    ParleyResult,
    to_builtin,
)
from .execution import (
    EventCancelSignal,
    ParleyCancelSignal,
    ParleyCancelSignalProtocol,
    ParleyExecution,
)
from .handler import ParleyCommandHandler, ParleyHandlerCodec
from .id_generator import DefaultIdGenerator, ParleyIdGenerator

__all__ = [
    "DefaultIdGenerator",
    "EventCancelSignal",
    "LoopClock",
    "ParleyBuiltinError",
    "ParleyCallOptions",
    "ParleyCancelSignal",
    "ParleyCancelSignalProtocol",
    "ParleyClock",
    "ParleyCommandHandler",
    "ParleyCommander",
    "ParleyCommanderCodec",
    "ParleyConfig",
    "ParleyConnectionAdapter",
    "ParleyError",
    "ParleyExecution",
    "ParleyHandlerCodec",
    "ParleyIdGenerator",
    "ParleyMessageHandler",
    "ParleyOutcome",
    "ParleyOutcomeKind",
    "ParleyResult",
    "ParleyTimerHandle",
    "ParleyTopics",
    "to_builtin",
]
