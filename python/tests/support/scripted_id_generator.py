"""A command-id generator that returns scripted ids in FIFO order (protocol.md §10)."""

from __future__ import annotations

from collections import deque


class ScriptedIdGenerator:
    """Bypasses the random §4.2 generator; returns the ids the scenario specifies, in order."""

    def __init__(self) -> None:
        self._ids: deque[str] = deque()

    def enqueue(self, command_id: str) -> None:
        self._ids.append(command_id)

    def new_id(self) -> str:
        if not self._ids:
            raise AssertionError("No scripted command id available.")
        return self._ids.popleft()
