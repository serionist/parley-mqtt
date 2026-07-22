"""The timer scheduler for one Parley instance over a :class:`~parley.clock.ParleyClock`.

Mirrors the TypeScript ``ParleyRuntime``: every scheduled timer is tracked so state stays bounded
(``live_timer_count`` is asserted by the bounded-state tests). asyncio is single-threaded per loop,
so no lock is needed; a cancelled clock timer never fires, so no generation guard is required —
:class:`ParleyTimer` keeps a ``disposed`` flag purely as belt-and-suspenders.
"""

from __future__ import annotations

from collections.abc import Callable

from .clock import ParleyClock, ParleyTimerHandle


class ParleyTimer:
    """A single logical timer: one-shot by default, rescheduled or cancelled explicitly."""

    __slots__ = ("_callback", "_clock", "_disposed", "_handle", "_runtime")

    def __init__(
        self,
        runtime: ParleyRuntime,
        clock: ParleyClock,
        delay_s: float,
        callback: Callable[[], None],
    ) -> None:
        self._runtime = runtime
        self._clock = clock
        self._callback = callback
        self._disposed = False
        runtime._increment()
        self._handle: ParleyTimerHandle = clock.schedule(self._fire, delay_s)

    def _fire(self) -> None:
        if self._disposed:
            return
        self._callback()

    def reschedule(self, delay_s: float) -> None:
        """Re-arm this timer to fire once more after ``delay_s``. Does not change the live count."""
        if self._disposed:
            return
        self._clock.cancel(self._handle)
        self._handle = self._clock.schedule(self._fire, delay_s)

    def cancel(self) -> None:
        """Stop and release the timer. Idempotent."""
        if self._disposed:
            return
        self._disposed = True
        self._clock.cancel(self._handle)
        self._runtime._decrement()


class ParleyRuntime:
    """The timer scheduler for one Parley instance over a :class:`ParleyClock`."""

    __slots__ = ("_clock", "_live_timers")

    def __init__(self, clock: ParleyClock) -> None:
        self._clock = clock
        self._live_timers = 0

    @property
    def live_timer_count(self) -> int:
        """Number of live (undisposed) timers."""
        return self._live_timers

    def schedule(self, delay_s: float, callback: Callable[[], None]) -> ParleyTimer:
        """Schedule a one-shot ``callback`` after ``delay_s`` seconds."""
        return ParleyTimer(self, self._clock, delay_s, callback)

    def _increment(self) -> None:
        self._live_timers += 1

    def _decrement(self) -> None:
        self._live_timers -= 1
