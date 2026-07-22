import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics } from '../src/index';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

describe('commander codec — encodeRequest (AB2)', () => {
  test('encodeRequest transforms the request payload before it is serialized on the wire', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<{ inner: number }, unknown, unknown, string>(
      transport,
      topics(),
      {},
      {
        idGenerator: gen,
        clock,
        codec: { encodeRequest: (value) => ({ wrapped: value }) },
      },
    );
    transport.clearOutbound();

    gen.enqueue('A');
    void commander.execute({ inner: 1 });

    const reqs = transport.publishes('request');
    expect(reqs).toHaveLength(1);
    expect(reqs[0]!.p).toEqual({ wrapped: { inner: 1 } });
  });

  test('a throwing encodeRequest surfaces synchronously out of execute and creates no command state', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      {},
      {
        idGenerator: gen,
        clock,
        codec: {
          encodeRequest() {
            throw new Error('cannot encode this request');
          },
        },
      },
    );
    transport.clearOutbound();

    gen.enqueue('A');
    // A throwing encodeRequest is a caller-visible programming error — it throws synchronously (like
    // the config/id validation) rather than resolving to any outcome, and leaves nothing behind.
    expect(() => commander.execute({})).toThrow('cannot encode this request');

    expect(transport.publishCount('request', 'req')).toBe(0);
    expect(commander._inflightCount).toBe(0);
    expect(commander._tombstoneCount).toBe(0);
    expect(commander._liveTimerCount).toBe(0);
  });
});
