"""Fixed-N event-loop drain (protocol.md §10 determinism, mirroring the TS microtask drain).

The Parley core's only asynchrony is ``await`` continuations of the per-command handler-execution
task; each :func:`asyncio.sleep` ``(0)`` advances the ready queue by one generation. A bounded loop of
``await asyncio.sleep(0)`` drains those continuations to quiescence WITHOUT advancing virtual time (the
injected :class:`VirtualClock` is independent), preserving the §10 timer-determinism equivalence. N=32
is far deeper than any chain the synchronous core creates (max depth 1–3).

What the drain enforces is virtual-clock determinism: every real Parley timer runs on the injected
:class:`VirtualClock`, so a publish the core defers behind a positive delay is ABSENT at the next
``expect`` until a step advances virtual time — it never rides a real-loop clock. The drain does NOT,
however, forbid same-instant real-loop hops: :func:`asyncio.sleep` ``(0)`` is a full loop iteration
(not a microtask flush), so a stray ``call_later(0)``/ready-queue hop would be absorbed within the
32 generations and stay wire-invisible. The stronger "no real-loop timing at all" property is
established by the sync-loopback and bounded-state tests plus review — not by this drain.
"""

from __future__ import annotations

import asyncio

_DRAIN_STEPS = 32


async def drain() -> None:
    """Drain up to 32 ready-queue generations without advancing virtual time."""
    for _ in range(_DRAIN_STEPS):
        await asyncio.sleep(0)
