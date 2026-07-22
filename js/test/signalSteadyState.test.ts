import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics, type ParleyAbortSignal } from '../src/index';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

/**
 * A duck-typed {@link ParleyAbortSignal} that counts every addEventListener / removeEventListener and
 * keeps the live listener set, so a leak on a long-lived shared signal is observable. It does NOT
 * auto-remove listeners when fired: the library must call removeEventListener on teardown, and this
 * test asserts it always does.
 */
class CountingSignal implements ParleyAbortSignal {
  aborted = false;
  addCount = 0;
  removeCount = 0;
  readonly #listeners = new Set<() => void>();

  addEventListener(_type: 'abort', listener: () => void): void {
    this.addCount++;
    this.#listeners.add(listener);
  }

  removeEventListener(_type: 'abort', listener: () => void): void {
    this.removeCount++;
    this.#listeners.delete(listener);
  }

  /** Trips the signal, firing every currently-registered listener (without removing it). */
  fire(): void {
    this.aborted = true;
    for (const listener of [...this.#listeners]) {
      listener();
    }
  }

  /** Re-arms the shared signal for the next call. */
  reset(): void {
    this.aborted = false;
  }

  get liveListeners(): number {
    return this.#listeners.size;
  }
}

describe('shared-signal listener steady state (TQ3)', () => {
  test('one long-lived signal across many sequential calls: add-count === remove-count, no leak', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      {
        requestRetryInterval: 2000,
        noAckTimeout: 1000,
        staleTimeout: 100_000,
        executionTimeout: 2000,
        cancelRetryInterval: 2000,
        tombstoneTtl: 500,
      },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    const signal = new CountingSignal();
    const ITERATIONS = 250;

    for (let i = 0; i < ITERATIONS; i++) {
      signal.reset(); // the ONE shared signal, re-armed each call
      const id = `c${i}`;
      gen.enqueue(id);
      void commander.execute({}, { signal });

      switch (i % 5) {
        case 0: // success
          transport.deliver('heartbeat', wire.heartbeat(id));
          transport.deliver('result', wire.resultOk(id, '{"r":1}'));
          break;
        case 1: // custom error
          transport.deliver('result', wire.resultError(id, 'domainErr'));
          break;
        case 2: // $noAck (never any sign of life) — a timeout outcome
          clock.advance(1001);
          break;
        case 3: // $executionTimeout — LIVE then the cap trips — another timeout outcome
          transport.deliver('heartbeat', wire.heartbeat(id));
          clock.advance(2001);
          break;
        case 4: // cancel: fire the shared signal mid-call, then a $cancelled result completes it
          signal.fire();
          transport.deliver('result', wire.resultError(id, '$cancelled'));
          break;
      }

      expect(commander._inflightCount).toBe(0); // each call terminates before the next starts
      expect(signal.liveListeners).toBe(0); // and releases its listener from the shared signal
      transport.clearOutbound();
    }

    clock.advance(1_000_000); // drain every remaining tombstone

    // The load-bearing assertion: across all outcomes (successes, errors, both timeout kinds, cancels)
    // every addEventListener was matched by a removeEventListener — no accumulation on the shared signal.
    expect(signal.addCount).toBeGreaterThan(0);
    expect(signal.removeCount).toBe(signal.addCount);
    expect(signal.liveListeners).toBe(0);

    expect(commander._inflightCount).toBe(0);
    expect(commander._tombstoneCount).toBe(0);
    expect(commander._liveTimerCount).toBe(0);
    expect(clock.pendingCount).toBe(0);
  });
});
