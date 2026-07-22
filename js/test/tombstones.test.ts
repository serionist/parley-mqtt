import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics } from '../src/index';
import { drainMicrotasks } from './support/drain';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { VectorHandler } from './support/vectorHandler';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

describe('tombstones (C5/H5/H8)', () => {
  test('commander tombstone expires after ttl', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { tombstoneTtl: 5000 },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    gen.enqueue('A');
    const p = commander.execute({});
    transport.deliver('result', wire.resultOk('A', '{"r":1}'));
    await p;

    expect(commander._inflightCount).toBe(0);
    expect(commander._tombstoneCount).toBe(1);

    clock.advanceTo(6000);
    expect(commander._tombstoneCount).toBe(0);
    expect(commander._liveTimerCount).toBe(0);
  });

  test('handler tombstone expires after ttl', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(transport, topics(), { tombstoneTtl: 5000 }, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A'));
    handler.complete('A', { r: 1 });
    await drainMicrotasks();
    transport.deliver('request', wire.resultAck('A'));

    expect(handler._commandCount).toBe(0);
    expect(handler._tombstoneCount).toBe(1);

    clock.advanceTo(6000);
    expect(handler._tombstoneCount).toBe(0);
    expect(handler._liveTimerCount).toBe(0);
  });

  test('commander tombstones evict oldest-first', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { maxTombstones: 2, tombstoneTtl: 3_600_000 },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    for (const id of ['A', 'B', 'C']) {
      gen.enqueue(id);
      const p = commander.execute({});
      transport.deliver('result', wire.resultOk(id, '{}'));
      await p;
    }

    expect(commander._tombstoneCount).toBe(2); // A (oldest) evicted when C tombstoned

    transport.clearOutbound();
    transport.deliver('result', wire.resultOk('A', '{}')); // A forgotten -> no rak (C6)
    expect(transport.publishCount('request', 'rak')).toBe(0);

    transport.deliver('result', wire.resultOk('C', '{}')); // C retained -> rak (C5)
    expect(transport.publishCount('request', 'rak')).toBe(1);
  });

  test('handler tombstones evict oldest-first', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(
      transport,
      topics(),
      { maxTombstones: 2, tombstoneTtl: 3_600_000, maxConcurrentExecutions: 8, maxActiveCommands: 8 },
      clock,
    );
    transport.clearOutbound();

    for (const id of ['A', 'B', 'C']) {
      transport.deliver('request', wire.request(id));
      handler.complete(id, {});
      await drainMicrotasks();
      transport.deliver('request', wire.resultAck(id));
    }

    expect(handler._tombstoneCount).toBe(2); // A (oldest) evicted when C tombstoned

    transport.clearOutbound();
    transport.deliver('request', wire.request('A')); // A forgotten -> re-admitted
    expect(handler._commandCount).toBe(1);
    expect(transport.publishCount('heartbeat', 'hb')).toBe(1);

    transport.clearOutbound();
    transport.deliver('request', wire.request('C')); // C tombstoned -> ignored
    expect(transport.publishCount('heartbeat', 'hb')).toBe(0);
  });
});
