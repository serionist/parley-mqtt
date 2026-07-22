"""The clock seam (design.md).

All timing in the core flows through :class:`ParleyClock` so the test suites can drive a
deterministic virtual clock while production defaults to the running event loop's timers. The core
never calls ``loop.call_later``/``loop.time()`` directly — only this injected interface.

Time is measured in **seconds** (float), matching :meth:`asyncio.AbstractEventLoop.time`. A handle is
opaque: whatever :meth:`ParleyClock.schedule` returns, only ever passed back to
:meth:`ParleyClock.cancel`.
"""

from __future__ import annotations

import asyncio
from collections.abc import Callable
from typing import Protocol, runtime_checkable

#: An opaque scheduling handle.
ParleyTimerHandle = object


@runtime_checkable
class ParleyClock(Protocol):
    """A minimal scheduling clock. Defaults to :class:`LoopClock`; overridden by the test suites."""

    def now(self) -> float:
        """Current time in seconds (monotonic-ish; the core does not depend on the epoch)."""
        ...

    def schedule(self, callback: Callable[[], None], delay_s: float) -> ParleyTimerHandle:
        """Schedule a one-shot ``callback`` after ``delay_s`` seconds, returning a handle."""
        ...

    def cancel(self, handle: ParleyTimerHandle) -> None:
        """Cancel a scheduled callback. A callback that has already fired is unaffected."""
        ...


class LoopClock:
    """The default production clock over the running :mod:`asyncio` event loop.

    Binds lazily to the running loop on first use, so a Parley instance may be constructed before
    (or in the same call as) the loop that will drive it, as long as all subsequent operations run
    on that loop.
    """

    __slots__ = ("_loop",)

    def __init__(self, loop: asyncio.AbstractEventLoop | None = None) -> None:
        self._loop = loop

    def _resolve_loop(self) -> asyncio.AbstractEventLoop:
        if self._loop is None:
            self._loop = asyncio.get_running_loop()
        return self._loop

    def now(self) -> float:
        return self._resolve_loop().time()

    def schedule(self, callback: Callable[[], None], delay_s: float) -> ParleyTimerHandle:
        return self._resolve_loop().call_later(delay_s, callback)

    def cancel(self, handle: ParleyTimerHandle) -> None:
        # Production handles are asyncio.TimerHandle; cancel() is idempotent.
        if isinstance(handle, asyncio.TimerHandle):
            handle.cancel()
