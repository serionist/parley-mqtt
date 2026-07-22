import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics } from '../src/index';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { VectorHandler } from './support/vectorHandler';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

describe('dispose', () => {
  test('commander dispose mid-flight completes $cancelled and clears state', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(transport, topics(), {}, {
      idGenerator: gen,
      clock,
    });
    transport.clearOutbound();

    gen.enqueue('A');
    const controller = new AbortController();
    const p = commander.execute({}, { signal: controller.signal });
    transport.deliver('heartbeat', wire.heartbeat('A')); // LIVE — sliding + execution-cap timers running

    commander.dispose();

    const result = await p;
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.builtin).toBe('$cancelled');
    }

    expect(commander._inflightCount).toBe(0);
    expect(commander._tombstoneCount).toBe(0);
    expect(commander._liveTimerCount).toBe(0);

    // Post-dispose API misuse throws; Dispose is idempotent.
    expect(() => commander.execute({})).toThrow(/disposed/);
    expect(() => {
      commander.dispose();
    }).not.toThrow();
  });

  test('handler dispose stops timers, signals cancellation, and clears state', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(transport, topics(), {}, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A')); // admitted + executing, heartbeat scheduled
    expect(handler._commandCount).toBe(1);
    expect(handler._liveTimerCount).toBeGreaterThanOrEqual(1);
    expect(handler.isCancelRequested('A')).toBe(false);

    handler.dispose();

    expect(handler.isCancelRequested('A')).toBe(true); // in-flight execution signalled
    expect(handler._commandCount).toBe(0);
    expect(handler._queueCount).toBe(0);
    expect(handler._executingCount).toBe(0);
    expect(handler._tombstoneCount).toBe(0);
    expect(handler._liveTimerCount).toBe(0);

    expect(() => {
      handler.dispose();
    }).not.toThrow(); // idempotent
  });

  test('post-dispose handler ignores inbound (no publish, no state)', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(transport, topics(), {}, clock);
    handler.dispose();
    transport.clearOutbound();

    transport.deliver('request', wire.request('B'));

    expect(transport.outbound).toHaveLength(0);
    expect(handler._commandCount).toBe(0);
  });
});
