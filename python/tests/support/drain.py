"""Fixed-N event-loop drain (protocol.md §10 determinism, mirroring the TS microtask drain).

The Parley core's only asynchrony is ``await`` continuations of the per-command handler-execution
task; each :func:`asyncio.sleep` ``(0)`` advances the ready queue by one generation. A bounded loop of
``await asyncio.sleep(0)`` drains those continuations to quiescence WITHOUT advancing virtual time (the
injected :class:`VirtualClock` is independent), preserving the §10 timer-determinism equivalence. N=32
is far deeper than any chain the synchronous core creates (max depth 1–3).

Draining only ready-queue generations — never a real-timer boundary — means a publish the library
(wrongly) deferred onto a real ``call_later(0)``/``loop`` hop is ABSENT at the next ``expect`` and the
vector fails: exactly the discipline the drain enforces.
"""

from __future__ import annotations

import asyncio

_DRAIN_STEPS = 32


async def drain() -> None:
    """Drain up to 32 ready-queue generations without advancing virtual time."""
    for _ in range(_DRAIN_STEPS):
        await asyncio.sleep(0)
