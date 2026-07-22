"""Shared cancel-signal listener steady state: add-count == remove-count, no leak (TQ3)."""

from __future__ import annotations

import asyncio
from collections.abc import Callable

from parley import ParleyCommander, ParleyConfig
from support import wire
from support.drain import drain
from support.fake_transport import FakeTransport
from support.harness import TOPICS
from support.scripted_id_generator import ScriptedIdGenerator
from support.virtual_clock import VirtualClock


class CountingSignal:
    """A :class:`~parley.ParleyCancelSignalProtocol` that counts add/remove and keeps the live set.

    It does NOT auto-remove listeners when fired — the library must ``remove_listener`` on teardown,
    which this test asserts across every outcome kind on one long-lived (re-armed) signal.
    """

    def __init__(self) -> None:
        self.aborted = False
        self.add_count = 0
        self.remove_count = 0
        self._listeners: set[Callable[[], None]] = set()

    def add_listener(self, listener: Callable[[], None]) -> None:
        self.add_count += 1
        self._listeners.add(listener)

    def remove_listener(self, listener: Callable[[], None]) -> None:
        self.remove_count += 1
        self._listeners.discard(listener)

    def fire(self) -> None:
        self.aborted = True
        for listener in list(self._listeners):
            listener()

    def reset(self) -> None:
        self.aborted = False

    @property
    def live_listeners(self) -> int:
        return len(self._listeners)


async def test_shared_signal_listener_parity_across_mixed_calls() -> None:
    clock = VirtualClock()
    transport = FakeTransport()
    gen = ScriptedIdGenerator()
    commander: ParleyCommander[object, object, object, str] = ParleyCommander(
        transport,
        TOPICS,
        ParleyConfig(
            request_retry_interval=2.0,
            no_ack_timeout=1.0,
            stale_timeout=100.0,
            execution_timeout=2.0,
            cancel_retry_interval=2.0,
            tombstone_ttl=0.5,
        ),
        id_generator=gen,
        clock=clock,
    )
    transport.clear_outbound()

    signal = CountingSignal()
    iterations = 250

    for i in range(iterations):
        signal.reset()  # the ONE shared signal, re-armed each call
        cid = f"c{i}"
        gen.enqueue(cid)
        task: asyncio.Task[object] = asyncio.ensure_future(commander.execute({}, signal=signal))
        await drain()

        kind = i % 5
        if kind == 0:  # success
            transport.deliver("heartbeat", wire.hb(cid))
            transport.deliver("result", wire.res_ok(cid, '{"r":1}'))
        elif kind == 1:  # custom error
            transport.deliver("result", wire.res_err(cid, "domainErr"))
        elif kind == 2:  # $noAck (never any sign of life)
            clock.advance_by_ms(1001)
        elif kind == 3:  # $executionTimeout (LIVE then the cap trips)
            transport.deliver("heartbeat", wire.hb(cid))
            clock.advance_by_ms(2001)
        else:  # cancel: fire the shared signal mid-call, then a $cancelled result completes it
            signal.fire()
            transport.deliver("result", wire.res_err(cid, "$cancelled"))

        await task
        assert commander._inflight_count == 0  # each call terminates before the next starts
        assert signal.live_listeners == 0  # and releases its listener from the shared signal
        transport.clear_outbound()

    clock.advance_by_ms(1_000_000)  # drain every remaining tombstone

    assert signal.add_count > 0
    assert signal.remove_count == signal.add_count  # no accumulation on the shared signal
    assert signal.live_listeners == 0
    assert commander._inflight_count == 0
    assert commander._tombstone_count == 0
    assert commander._live_timer_count == 0
    assert clock.pending_count == 0
