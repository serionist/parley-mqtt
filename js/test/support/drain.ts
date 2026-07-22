/**
 * TQ1 — flushes ONLY the microtask queue, with zero real-timer (macrotask) boundaries. The Parley
 * core is microtask-only in its dispatch glue: the sole asynchrony between an inbound event (or a
 * handler resolution) and the resulting publish is `await` continuations. A bounded loop of
 * `await Promise.resolve()` drains those continuations — including the shallow chains the library
 * produces — to quiescence.
 *
 * Using real `setTimeout(0)` here would drain the microtask queue *for free* before each macrotask
 * callback, which would silently absorb a publish that the library had (wrongly) deferred onto a
 * macrotask. Draining microtasks only means such a publish is ABSENT at the next `expect` and the
 * vector fails — that is the point: it enforces the §5 microtask-only discipline. 32 iterations is
 * far deeper than any chain the core creates.
 *
 * This advances no virtual time (the injected VirtualClock is independent of this loop), so the §10
 * timer-determinism equivalence is preserved.
 */
export async function drainMicrotasks(): Promise<void> {
  for (let i = 0; i < 32; i++) {
    await Promise.resolve();
  }
}
