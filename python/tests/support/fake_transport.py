"""A fake in-memory transport: records every publish and subscribe, delivers scripted inbound
messages to the library's per-topic handlers, and can fire the connected signal. No broker.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from dataclasses import dataclass

from parley import ParleyConnectionAdapter, ParleyMessageHandler


@dataclass(frozen=True)
class PublishAction:
    """A recorded publish: topic + payload bytes."""

    topic: str
    payload: bytes


@dataclass(frozen=True)
class SubscribeAction:
    """A recorded subscribe: topic."""

    topic: str


OutboundAction = object  # PublishAction | SubscribeAction (kept broad for the runner's dumps)


class FakeTransport(ParleyConnectionAdapter):
    """In-memory transport with an optional synchronous-loopback hook (design.md)."""

    def __init__(self) -> None:
        super().__init__()
        self._handlers: dict[str, ParleyMessageHandler] = {}
        self.outbound: list[object] = []
        self._reacting = False
        #: When True, :meth:`publish` raises — simulates a disconnected/disposed client.
        self.fail_publish = False
        #: Optional synchronous-loopback hook, invoked from INSIDE publish (after recording), so a
        #: test can synchronously deliver an inbound message, re-entering the library mid-transition.
        #: Re-entrant publishes made from within the hook do not re-trigger it (single-level).
        self.on_publish: Callable[[str, bytes], None] | None = None

    def publish(self, topic: str, payload: bytes) -> None:
        if self.fail_publish:
            raise RuntimeError("FakeTransport: publish failed (simulated adapter fault).")
        self.outbound.append(PublishAction(topic, payload))

        reaction = self.on_publish
        if reaction is not None and not self._reacting:
            self._reacting = True
            try:
                reaction(topic, payload)
            finally:
                self._reacting = False

    def subscribe(self, topic: str, on_message: ParleyMessageHandler) -> None:
        self._handlers[topic] = on_message
        self.outbound.append(SubscribeAction(topic))

    def unsubscribe(self, topic: str) -> None:
        self._handlers.pop(topic, None)

    def deliver(self, topic: str, payload: bytes) -> None:
        """Deliver a raw payload to the handler subscribed on ``topic``, if any."""
        handler = self._handlers.get(topic)
        if handler is not None:
            handler(topic, payload)

    def fire_connected(self) -> None:
        """Fire the transport-connected signal (a reconnection)."""
        self.raise_connected()

    def clear_outbound(self) -> None:
        """Discard recorded outbound actions (used to exclude construction-time subscribes)."""
        self.outbound = []

    # --- convenience helpers for unit tests ---
    def publishes(self, topic: str) -> list[dict[str, object]]:
        """Parsed publish payloads on a topic."""
        return [
            json.loads(a.payload.decode("utf-8"))
            for a in self.outbound
            if isinstance(a, PublishAction) and a.topic == topic
        ]

    def publish_count(self, topic: str, msg_type: str) -> int:
        """Count of publishes of a given message type on a topic."""
        return sum(1 for m in self.publishes(topic) if m.get("t") == msg_type)

    def subscribe_count(self, topic: str) -> int:
        """Count of subscribes recorded for a topic."""
        return sum(1 for a in self.outbound if isinstance(a, SubscribeAction) and a.topic == topic)
