import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics } from '../src/index';
import { drainMicrotasks } from './support/drain';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { VectorHandler } from './support/vectorHandler';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

const decoder = new TextDecoder();
const typeOf = (payload: Uint8Array): string =>
  (JSON.parse(decoder.decode(payload)) as { t?: string }).t ?? '';

/**
 * CR1 regression — "Synchronous delivery is legal; publish comes last" (design.md). A loopback
 * adapter MAY deliver an inbound message synchronously from inside `publish`, re-entering the library
 * mid-transition. Before the schedule-before-publish fixes, that re-entrant reply tore the command
 * down while the transition still scheduled its timers AFTER the publish returned — orphaning
 * self-rearming timers (a req-retry / cnl / res storm that state could never drain). These tests use
 * FakeTransport's `onPublish` sync hook to force exactly that re-entrancy at each of the three sites.
 */
describe('synchronous-loopback re-entrancy (CR1)', () => {
  test('(a) hb delivered synchronously from the req publish → no stray req retries, correct stale window', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { requestRetryInterval: 2000, noAckTimeout: 5000, staleTimeout: 20_000 },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    // When the commander publishes the initial `req`, synchronously deliver an `hb` (the accept-ack).
    transport.onPublish = (topic, payload) => {
      if (topic === 'request' && typeOf(payload) === 'req') {
        transport.deliver('heartbeat', wire.heartbeat('A'));
      }
    };

    gen.enqueue('A');
    const p = commander.execute({});

    // One req published; the synchronous hb moved the command to LIVE from inside that publish.
    expect(transport.publishCount('request', 'req')).toBe(1);
    expect(commander._inflightCount).toBe(1);

    // Advance past requestRetryInterval AND past noAckTimeout, but under staleTimeout. Post-fix the
    // req-retry timer was cancelled by the synchronous hb, and the sign-of-life window was correctly
    // re-armed to staleTimeout — so nothing fires. (Pre-fix: an orphaned req-retry storm, and the
    // window wrongly left at noAckTimeout would trip $executionStale at t=5000.)
    transport.clearOutbound();
    clock.advanceTo(6000);
    expect(transport.publishCount('request', 'req')).toBe(0);
    expect(commander._inflightCount).toBe(1);

    // A genuine result still completes it cleanly.
    transport.deliver('result', wire.resultOk('A', '{"r":1}'));
    const result = await p;
    expect(result.ok).toBe(true);
  });

  test('(b) $cancelled res delivered synchronously from the cnl publish → no orphaned cancel-retry timer', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { requestRetryInterval: 2000, noAckTimeout: 100_000, cancelRetryInterval: 2000, tombstoneTtl: 5000 },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    // When the commander publishes a `cnl`, synchronously deliver the handler's `$cancelled` res.
    transport.onPublish = (topic, payload) => {
      if (topic === 'request' && typeOf(payload) === 'cnl') {
        transport.deliver('result', wire.resultError('A', '$cancelled'));
      }
    };

    gen.enqueue('A');
    const controller = new AbortController();
    const p = commander.execute({}, { signal: controller.signal });
    controller.abort(); // → cnl published → reaction delivers $cancelled synchronously → teardown

    const result = await p;
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.code).toBe('$cancelled');
    }

    // Exactly one cnl (the initial one) — the cancel-retry timer was scheduled BEFORE the publish, so
    // the re-entrant teardown cancelled it. Draining the clock returns to zero live timers and emits
    // no further cnl. (Pre-fix: an orphaned self-rearming cancel-retry timer → a permanent cnl storm.)
    expect(transport.publishCount('request', 'cnl')).toBe(1);
    expect(commander._inflightCount).toBe(0);

    transport.clearOutbound();
    clock.advanceTo(1_000_000);
    expect(commander._liveTimerCount).toBe(0);
    expect(commander._tombstoneCount).toBe(0);
    expect(transport.publishCount('request', 'cnl')).toBe(0);
  });

  test('(c) rak delivered synchronously from the res publish → no orphaned result timers', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(
      transport,
      topics(),
      { heartbeatInterval: 1000, resultRetryInterval: 2000, resultRetryTimeout: 30_000, tombstoneTtl: 5000 },
      clock,
    );
    transport.clearOutbound();

    // When the handler publishes the `res`, synchronously deliver the commander's `rak`.
    transport.onPublish = (topic, payload) => {
      if (topic === 'result' && typeOf(payload) === 'res') {
        transport.deliver('request', wire.resultAck('A'));
      }
    };

    transport.deliver('request', wire.request('A')); // admit → executing (first hb on heartbeat topic)
    expect(handler._executingCount).toBe(1);

    handler.complete('A', { r: 1 });
    await drainMicrotasks(); // completeWithResult schedules result timers, publishes res → sync rak → tombstone

    // The synchronous rak moved the command straight to a tombstone; the result-retry + timeout timers
    // were scheduled BEFORE the publish, so the re-entrant transition found and cancelled them.
    expect(handler._commandCount).toBe(0);
    expect(handler._executingCount).toBe(0);
    expect(handler._tombstoneCount).toBe(1);
    expect(handler._liveTimerCount).toBe(1); // only the tombstone TTL timer remains
    expect(transport.publishCount('result', 'res')).toBe(1);

    transport.clearOutbound();
    clock.advanceTo(1_000_000);
    expect(handler._liveTimerCount).toBe(0);
    expect(handler._tombstoneCount).toBe(0);
    expect(transport.publishCount('result', 'res')).toBe(0); // no post-tombstone res storm
  });
});
