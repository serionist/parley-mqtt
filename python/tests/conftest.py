"""Shared fixtures.

The ``loop_error_guard`` autouse fixture installs a capturing event-loop exception handler so that
any "Task was destroyed but it is pending" / "exception was never retrieved" event fails the test —
the loop-side half of the zero-warnings gate (the warnings-module half is ``filterwarnings=["error"]``
in ``pyproject.toml``).
"""

from __future__ import annotations

import asyncio
import gc

import pytest_asyncio


@pytest_asyncio.fixture(autouse=True)
async def loop_error_guard() -> object:
    loop = asyncio.get_running_loop()
    captured: list[dict[str, object]] = []

    def handler(_loop: asyncio.AbstractEventLoop, context: dict[str, object]) -> None:
        captured.append(context)

    loop.set_exception_handler(handler)
    yield
    # Surface any pending __del__ (leaked pending tasks / unretrieved exceptions) before asserting.
    await asyncio.sleep(0)
    gc.collect()
    await asyncio.sleep(0)
    messages = [str(ctx.get("message", ctx)) for ctx in captured]
    assert not captured, f"event-loop exception(s) captured: {messages}"
