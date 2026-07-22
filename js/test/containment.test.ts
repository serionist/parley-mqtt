import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics } from '../src/index';
import { drainMicrotasks } from './support/drain';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { VectorHandler } from './support/vectorHandler';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

describe('adapter-fault containment', () => {
  test('handler: a throwing publish is swallowed and retries self-heal', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(
      transport,
      topics(),
      { resultRetryInterval: 2000, resultRetryTimeout: 300_000 },
      clock,
    );
    transport.clearOutbound();

    transport.deliver('request', wire.request('A'));
    handler.complete('A', {}); // publishes res, schedules the retry cadence
    await drainMicrotasks();
    transport.clearOutbound();

    // A retry fires while the adapter is down: the throw must be swallowed, not escape.
    transport.failPublish = true;
    expect(() => {
      clock.advanceTo(2000);
    }).not.toThrow();
    expect(transport.publishCount('result', 'res')).toBe(0); // publish threw, nothing recorded

    // Once the adapter recovers, the next retry publishes — the cadence survived (self-healing).
    transport.failPublish = false;
    clock.advanceTo(4000);
    expect(transport.publishCount('result', 'res')).toBeGreaterThanOrEqual(1);
  });

  test('commander: a throwing publish during req retry is swallowed and self-heals', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      { requestRetryInterval: 2000, noAckTimeout: 100_000 },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    gen.enqueue('A');
    void commander.execute({});
    transport.clearOutbound();

    transport.failPublish = true;
    expect(() => {
      clock.advanceTo(2000);
    }).not.toThrow();
    expect(transport.publishCount('request', 'req')).toBe(0);

    transport.failPublish = false;
    clock.advanceTo(4000);
    expect(transport.publishCount('request', 'req')).toBeGreaterThanOrEqual(1);
  });

  test('commander: a throwing onProgress callback is contained', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(transport, topics(), {}, {
      idGenerator: gen,
      clock,
    });
    transport.clearOutbound();

    gen.enqueue('A');
    const p = commander.execute(
      {},
      {
        onProgress() {
          throw new Error('consumer progress callback blew up');
        },
      },
    );

    // The progress delivery must not throw out of the transport callback, and the call proceeds.
    expect(() => {
      transport.deliver('progress', wire.progress('A', '{"step":1}'));
    }).not.toThrow();

    transport.deliver('result', wire.resultOk('A', '{"r":1}'));
    const result = await p;
    expect(result.ok).toBe(true);
  });
});
