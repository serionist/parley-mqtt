"""A bounded set of tombstoned command ids (protocol.md C5 / H5 / H8).

Each entry self-expires after its TTL, and the table is additionally hard-capped with oldest-first
eviction, so state stays bounded on both axes. A :class:`dict` preserves insertion order, giving
oldest-first for free.
"""

from __future__ import annotations

from ._runtime import ParleyRuntime, ParleyTimer


class TombstoneTable:
    """A TTL'd, hard-capped set of command ids with oldest-first eviction."""

    __slots__ = ("_capacity", "_entries", "_runtime")

    def __init__(self, runtime: ParleyRuntime, capacity: int) -> None:
        self._runtime = runtime
        self._capacity = capacity
        self._entries: dict[str, ParleyTimer] = {}

    @property
    def count(self) -> int:
        return len(self._entries)

    def has(self, command_id: str) -> bool:
        return command_id in self._entries

    def add(self, command_id: str, ttl_s: float) -> None:
        # Refresh if already present (cancel the old TTL timer and re-insert at the end).
        existing = self._entries.pop(command_id, None)
        if existing is not None:
            existing.cancel()

        timer = self._runtime.schedule(ttl_s, lambda: self.remove(command_id))
        self._entries[command_id] = timer

        # H8: oldest-first eviction when over the hard cap (dict iterates in insertion order).
        while len(self._entries) > self._capacity:
            oldest = next(iter(self._entries))
            self.remove(oldest)

    def remove(self, command_id: str) -> None:
        entry = self._entries.pop(command_id, None)
        if entry is not None:
            entry.cancel()

    def clear(self) -> None:
        """Cancel every entry's TTL timer and empty the table (used on close)."""
        for entry in self._entries.values():
            entry.cancel()
        self._entries.clear()
