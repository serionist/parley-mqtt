"""Small conveniences shared by the unit suite."""

from __future__ import annotations

import asyncio
from typing import Any

from parley import (
    ParleyCallOptions,
    ParleyCancelSignal,
    ParleyCommander,
    ParleyResult,
    ParleyTopics,
)

from .drain import drain
from .scripted_id_generator import ScriptedIdGenerator

#: The standard opaque topic set used across the unit suite.
TOPICS = ParleyTopics("request", "heartbeat", "progress", "result")


async def start_call(
    commander: ParleyCommander[Any, Any, Any, str],
    id_generator: ScriptedIdGenerator,
    command_id: str,
    payload: object = None,
    *,
    options: ParleyCallOptions | None = None,
    signal: ParleyCancelSignal | None = None,
) -> asyncio.Task[ParleyResult[Any, str]]:
    """Start ``commander.execute`` under a scripted id and drain so the initial ``req`` is published."""
    id_generator.enqueue(command_id)
    task: asyncio.Task[ParleyResult[Any, str]] = asyncio.ensure_future(
        commander.execute(payload, options=options, signal=signal)
    )
    await drain()
    return task
