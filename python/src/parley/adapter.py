"""The transport port the consumer implements over their own MQTT client (design.md).

Parley never connects, authenticates, or configures TLS; it is handed a live session (protocol.md
§2). All publishes MUST use ``retain=false`` and MAY be QoS 0 or 1 — the protocol is correct at QoS
0. ``subscribe`` has **replace** semantics: subscribing a topic replaces any prior handler for it
(one handler per topic). The library re-subscribes every topic on each connected signal without
unsubscribing first, so an appending adapter would multiply-deliver.
"""

from __future__ import annotations

import contextlib
from abc import ABC, abstractmethod
from collections.abc import Callable

#: Delivered inbound payload callback: ``(topic, payload_bytes)``.
ParleyMessageHandler = Callable[[str, bytes], None]


class ParleyConnectionAdapter(ABC):
    """Abstract transport adapter; the consumer implements it over a concrete MQTT client."""

    __slots__ = ("_connected_callbacks",)

    def __init__(self) -> None:
        self._connected_callbacks: list[Callable[[], None]] = []

    @abstractmethod
    def publish(self, topic: str, payload: bytes) -> None:
        """Publish a UTF-8 JSON payload to a topic (fire-and-forget, ``retain=false``)."""

    @abstractmethod
    def subscribe(self, topic: str, on_message: ParleyMessageHandler) -> None:
        """Subscribe to a topic; replace any prior handler for it (one handler per topic)."""

    @abstractmethod
    def unsubscribe(self, topic: str) -> None:
        """Unsubscribe from a topic."""

    def on_connected(self, callback: Callable[[], None]) -> None:
        """Register a callback fired on every (re)connection. The library re-subscribes in response."""
        if callback not in self._connected_callbacks:
            self._connected_callbacks.append(callback)

    def off_connected(self, callback: Callable[[], None]) -> None:
        """Detach a previously registered connected callback."""
        with contextlib.suppress(ValueError):
            self._connected_callbacks.remove(callback)

    def raise_connected(self) -> None:
        """Adapter implementations call this on every (re)connection."""
        for callback in list(self._connected_callbacks):
            callback()
