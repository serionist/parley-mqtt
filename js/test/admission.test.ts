import { describe, expect, test } from 'vitest';
import { ParleyTopics } from '../src/index';
import { FakeTransport } from './support/fakeTransport';
import { VirtualClock } from './support/virtualClock';
import { VectorHandler } from './support/vectorHandler';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

/** A transport that re-delivers a scripted req during its first heartbeat publish (re-entrancy). */
class ReentrantTransport extends FakeTransport {
  reentrantReq?: Uint8Array;
  #fired = false;

  override publish(topic: string, payload: Uint8Array): void {
    super.publish(topic, payload);
    if (!this.#fired && this.reentrantReq && topic === 'heartbeat') {
      this.#fired = true;
      this.deliver('request', this.reentrantReq);
    }
  }
}

describe('admission and pool (H1/H2/H8)', () => {
  test('synchronous re-entrant duplicate req admits exactly once', () => {
    // JS is single-threaded, but a pathological adapter can synchronously re-enter the state machine
    // mid-dispatch. Admission (mutate-before-publish + dedup-first) must stay atomic.
    for (let i = 0; i < 5; i++) {
      const clock = new VirtualClock();
      const transport = new ReentrantTransport();
      const handler = new VectorHandler(transport, topics(), {}, clock);
      transport.clearOutbound();

      transport.reentrantReq = wire.request('A');
      transport.deliver('request', wire.request('A'));

      expect(handler._commandCount).toBe(1); // dedup + capacity + admit is atomic
      expect(handler._executingCount).toBe(1);
      expect(transport.publishCount('result', 'res')).toBe(0); // never $busy for a duplicate
      expect(transport.publishCount('heartbeat', 'hb')).toBe(2); // admission hb + H1 duplicate hb
    }
  });

  test('pool overflow yields stateless $busy without admitting', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(transport, topics(), { maxActiveCommands: 2 }, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A')); // admitted, executing
    transport.deliver('request', wire.request('B')); // admitted, queued (serial handler)
    transport.clearOutbound();

    transport.deliver('request', wire.request('C')); // pool full -> stateless $busy

    expect(handler._commandCount).toBe(2);
    expect(handler._executingCount).toBe(1);
    expect(handler._queueCount).toBe(1);
    expect(handler._tombstoneCount).toBe(0); // $busy creates no state

    const busy = transport.publishes('result');
    expect(busy).toHaveLength(1);
    expect(busy[0]!.id).toBe('C');
    expect(busy[0]!.e).toBe('$busy');
  });

  test('duplicate of admitted id on a full pool gets a heartbeat, not $busy (dedup precedes capacity)', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    new VectorHandler(transport, topics(), { maxActiveCommands: 1 }, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A')); // admitted, pool now full
    transport.clearOutbound();

    transport.deliver('request', wire.request('A')); // duplicate of admitted id

    expect(transport.publishCount('heartbeat', 'hb')).toBe(1);
    expect(transport.publishCount('result', 'res')).toBe(0); // not $busy
  });
});
