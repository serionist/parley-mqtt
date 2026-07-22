"""A deterministic virtual clock satisfying the protocol.md §10 determinism rule.

On :meth:`advance_to_ms`, every timer due at or before the target fires in due-order (insertion order
on ties) before time settles at the target. Advancing is the only thing that moves virtual time;
draining microtasks (:mod:`drain`) never touches it.

Time is kept internally as an **integer number of microseconds** so that the millisecond-granular
vector times and config durations never accumulate binary-float drift (the :class:`ParleyClock`
seam works in seconds, and ``ms / 1000 * 1_000_000`` round-trips exactly through ``round``).
"""

from __future__ import annotations

from collections.abc import Callable

from parley import ParleyTimerHandle

_SCALE = 1_000_000  # microseconds per second


class _Timer:
    __slots__ = ("callback", "due_us", "handle", "seq")

    def __init__(self, handle: int, due_us: int, seq: int, callback: Callable[[], None]) -> None:
        self.handle = handle
        self.due_us = due_us
        self.seq = seq
        self.callback = callback


class VirtualClock:
    """A deterministic :class:`~parley.ParleyClock` for the test suites."""

    def __init__(self) -> None:
        self._now_us = 0
        self._seq = 0
        self._timers: dict[int, _Timer] = {}

    def now(self) -> float:
        return self._now_us / _SCALE

    def schedule(self, callback: Callable[[], None], delay_s: float) -> ParleyTimerHandle:
        handle = self._seq
        self._seq += 1
        due_us = self._now_us + round(delay_s * _SCALE)
        self._timers[handle] = _Timer(handle, due_us, handle, callback)
        return handle

    def cancel(self, handle: ParleyTimerHandle) -> None:
        if isinstance(handle, int):
            self._timers.pop(handle, None)

    @property
    def pending_count(self) -> int:
        """Number of pending (uncancelled, unfired) timers — sanity check for the runner."""
        return len(self._timers)

    def advance_to_ms(self, target_ms: float) -> None:
        """Advance virtual time to ``target_ms`` (milliseconds).

        Fires every timer due at or before it in ``(due_us, seq)`` order. A callback that
        reschedules/schedules a timer still due at or before the target fires within the same advance.
        Time never moves backward.
        """
        self._advance_to_us(round(target_ms * (_SCALE // 1000)))

    def advance_by_ms(self, delta_ms: float) -> None:
        """Advance virtual time by a relative ``delta_ms`` (milliseconds)."""
        self._advance_to_us(self._now_us + round(delta_ms * (_SCALE // 1000)))

    def _advance_to_us(self, target_us: int) -> None:
        if target_us < self._now_us:
            raise AssertionError(
                f"VirtualClock cannot move backward (now={self._now_us}us, target={target_us}us)."
            )

        while True:
            nxt: _Timer | None = None
            for timer in self._timers.values():
                if timer.due_us > target_us:
                    continue
                if nxt is None or (timer.due_us, timer.seq) < (nxt.due_us, nxt.seq):
                    nxt = timer
            if nxt is None:
                break
            del self._timers[nxt.handle]
            if nxt.due_us > self._now_us:
                self._now_us = nxt.due_us
            nxt.callback()

        self._now_us = target_us
