import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics } from '../src/index';
import { drainMicrotasks } from './support/drain';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { VectorHandler } from './support/vectorHandler';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

const CYCLES = 3000;

// Microtask-only flush (TQ1): the handler dispatch adds one microtask hop from outcome resolution to
// the res publish and no macrotask boundary; draining microtasks is sufficient and enforces that.
function flush(): Promise<void> {
  return drainMicrotasks();
}

describe('bounded state (hard requirement — testing.md layer 1)', () => {
  test('commander: sustained mixed load returns to steady state', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const gen = new ScriptedIdGenerator();
    const commander = new ParleyCommander<unknown, unknown, unknown, string>(
      transport,
      topics(),
      {
        requestRetryInterval: 1000,
        noAckTimeout: 2500,
        staleTimeout: 2500,
        executionTimeout: 10_000,
        cancelRetryInterval: 1000,
        tombstoneTtl: 3000,
      },
      { idGenerator: gen, clock },
    );
    transport.clearOutbound();

    for (let i = 0; i < CYCLES; i++) {
      const id = `c${i}`;
      gen.enqueue(id);
      const controller = new AbortController();
      void commander.execute({}, { signal: controller.signal });

      switch (i % 5) {
        case 0: // success
          transport.deliver('heartbeat', wire.heartbeat(id));
          transport.deliver('result', wire.resultOk(id, '{"r":1}'));
          break;
        case 1: // custom failure
          transport.deliver('result', wire.resultError(id, 'domainErr'));
          break;
        case 2: // $busy
          transport.deliver('result', wire.resultError(id, '$busy'));
          break;
        case 3: // $noAck timeout (never any sign of life)
          clock.advance(2600);
          break;
        case 4: // cancel, then a $cancelled result wins
          controller.abort();
          transport.deliver('result', wire.resultError(id, '$cancelled'));
          break;
      }

      expect(commander._inflightCount).toBe(0); // each cycle terminates before the next
      transport.clearOutbound();
      clock.advance(1);
    }

    clock.advance(5000); // drain all tombstones

    expect(commander._inflightCount).toBe(0);
    expect(commander._tombstoneCount).toBe(0);
    expect(commander._liveTimerCount).toBe(0);
    expect(clock.pendingCount).toBe(0); // TQ4: no timer left registered on the clock either
  });

  test('handler: sustained mixed load returns to steady state', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const config = {
      heartbeatInterval: 1000,
      resultRetryInterval: 1000,
      resultRetryTimeout: 2500,
      tombstoneTtl: 3000,
      maxConcurrentExecutions: 4,
      maxActiveCommands: 8,
    };
    const handler = new VectorHandler(transport, topics(), config, clock);
    transport.clearOutbound();

    for (let i = 0; i < CYCLES; i++) {
      const id = `h${i}`;
      transport.deliver('request', wire.request(id));

      switch (i % 6) {
        case 0:
        case 5: // complete + rak
          handler.complete(id, {});
          await flush();
          transport.deliver('request', wire.resultAck(id));
          break;
        case 1: // custom fail + rak
          handler.fail(id, 'domainErr');
          await flush();
          transport.deliver('request', wire.resultAck(id));
          break;
        case 2: // complete, no rak -> resultRetryTimeout give-up -> tombstone
          handler.complete(id, {});
          await flush();
          clock.advance(2600);
          break;
        case 3: // cancel executing -> $cancelled + rak
          transport.deliver('request', wire.cancel(id));
          handler.fail(id, '$cancelled');
          await flush();
          transport.deliver('request', wire.resultAck(id));
          break;
        case 4: // stateless cancel of an unknown id, then complete this one
          transport.deliver('request', wire.cancel(`ghost${i}`));
          handler.complete(id, {});
          await flush();
          transport.deliver('request', wire.resultAck(id));
          break;
      }

      transport.clearOutbound();
      clock.advance(1);
    }

    // A few $busy bursts: fill the pool with parked commands, overflow, then drain.
    for (let burst = 0; burst < 40; burst++) {
      const parked: string[] = [];
      for (let k = 0; k < config.maxActiveCommands; k++) {
        const id = `b${burst}_${k}`;
        transport.deliver('request', wire.request(id));
        parked.push(id);
      }

      transport.deliver('request', wire.request(`overflow${burst}`)); // pool full -> stateless $busy
      expect(handler._commandCount).toBe(config.maxActiveCommands);

      const remaining = new Set(parked);
      while (remaining.size > 0) {
        let progressed = false;
        for (const id of [...remaining]) {
          if (handler.isExecuting(id)) {
            handler.complete(id, {});
            await flush();
            transport.deliver('request', wire.resultAck(id));
            remaining.delete(id);
            progressed = true;
          }
        }
        expect(progressed).toBe(true); // drain must not stall
      }

      transport.clearOutbound();
      clock.advance(1);
    }

    clock.advance(5000); // drain all tombstones and any lingering result-retry state

    expect(handler._commandCount).toBe(0);
    expect(handler._queueCount).toBe(0);
    expect(handler._executingCount).toBe(0);
    expect(handler._tombstoneCount).toBe(0);
    expect(handler._liveTimerCount).toBe(0);
    expect(clock.pendingCount).toBe(0); // TQ4: no timer left registered on the clock either
  }, 60_000);
});
