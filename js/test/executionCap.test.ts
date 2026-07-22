import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics, type ParleyResult } from '../src/index';
import { drainMicrotasks } from './support/drain';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

// A very large stale window keeps the sliding sign-of-life timer from firing, isolating the cap.
const HUGE_STALE = 100_000_000;

describe('execution cap disable (C2/C3, tri-state)', () => {
  test('executionTimeout: null at config level disables the cap', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { executionTimeout: null, staleTimeout: HUGE_STALE },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    gen.enqueue('A');
    const record: { settled: boolean; value?: ParleyResult<unknown, string> } = { settled: false };
    void commander.execute({}).then((value) => {
      record.settled = true;
      record.value = value;
    });
    transport.deliver('heartbeat', wire.heartbeat('A')); // LIVE — cap would start here if enabled

    clock.advanceTo(1_000_000); // far past the default 60_000 cap
    await drainMicrotasks();
    expect(record.settled).toBe(false); // no $executionTimeout — the cap is disabled

    transport.deliver('result', wire.resultOk('A', '{"r":1}'));
    await drainMicrotasks();
    expect(record.settled).toBe(true);
    expect(record.value?.ok).toBe(true);
  });

  test('per-call executionTimeout: null overrides a configured default (disables the cap)', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { executionTimeout: 5000, staleTimeout: HUGE_STALE }, // a cap IS configured
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    gen.enqueue('A');
    const record: { settled: boolean; value?: ParleyResult<unknown, string> } = { settled: false };
    void commander.execute({}, { executionTimeout: null }).then((value) => {
      record.settled = true;
      record.value = value;
    });
    transport.deliver('heartbeat', wire.heartbeat('A'));

    clock.advanceTo(1_000_000); // well past the configured 5000 cap
    await drainMicrotasks();
    expect(record.settled).toBe(false); // per-call null won — no $executionTimeout

    transport.deliver('result', wire.resultOk('A', '{"r":1}'));
    await drainMicrotasks();
    expect(record.value?.ok).toBe(true);
  });

  test('control: a configured cap still fires when not disabled', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { executionTimeout: 5000, staleTimeout: HUGE_STALE },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    gen.enqueue('A');
    const p = commander.execute({});
    transport.deliver('heartbeat', wire.heartbeat('A'));

    clock.advanceTo(5000);
    const result = await p;
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.code).toBe('$executionTimeout');
    }
  });
});
