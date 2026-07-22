import { describe, expect, test } from 'vitest';
import {
  ParleyCommander,
  ParleyCommandHandler,
  ParleyOutcome,
  ParleyTopics,
  type ParleyExecution,
} from '../src/index';
import { drainMicrotasks } from './support/drain';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

function newCommander<TResult>(codec?: { decodeResult?(raw: unknown): TResult }): {
  transport: FakeTransport;
  gen: ScriptedIdGenerator;
  commander: ParleyCommander<unknown, unknown, TResult, string>;
} {
  const clock = new VirtualClock();
  const transport = new FakeTransport();
  const gen = new ScriptedIdGenerator();
  const commander = new ParleyCommander<unknown, unknown, TResult, string>(transport, topics(), {}, {
    idGenerator: gen,
    clock,
    ...(codec ? { codec } : {}),
  });
  transport.clearOutbound();
  return { transport, gen, commander };
}

describe('error typing (§5)', () => {
  test('builtin error is surfaced as builtin', async () => {
    const { transport, gen, commander } = newCommander();
    gen.enqueue('A');
    const p = commander.execute({});
    transport.deliver('result', wire.resultError('A', '$busy'));

    const result = await p;
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.builtin).toBe('$busy');
      expect(result.error.custom).toBeUndefined();
      expect(result.error.code).toBe('$busy');
    }
  });

  test('custom error is surfaced typed with its message', async () => {
    const { transport, gen, commander } = newCommander();
    gen.enqueue('A');
    const p = commander.execute({});
    transport.deliver('result', wire.json({ v: 1, t: 'res', id: 'A', ok: false, e: 'diskFull', m: 'disk is full' }));

    const result = await p;
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.builtin).toBeUndefined();
      expect(result.error.code).toBe('diskFull');
      expect(result.error.custom).toBe('diskFull');
      expect(result.error.message).toBe('disk is full');
    }
  });

  test('success payload that fails to decode yields $decodeError but still acks (C4)', async () => {
    const { transport, gen, commander } = newCommander<number>({
      decodeResult(raw): number {
        if (typeof raw !== 'number') {
          throw new Error('expected a number');
        }
        return raw;
      },
    });
    gen.enqueue('A');
    const p = commander.execute({});
    transport.deliver('result', wire.resultOk('A', '"not-a-number"')); // ok:true, but p is a string

    // The result WAS delivered — rak is still published (redelivery cannot fix a contract mismatch)...
    expect(transport.publishCount('request', 'rak')).toBe(1);

    // ...and the call terminates locally with the commander-local builtin $decodeError.
    const result = await p;
    expect(result.ok).toBe(false);
    if (!result.ok) {
      expect(result.error.builtin).toBe('$decodeError');
      expect(result.error.code).toBe('$decodeError');
    }
  });
});

/** A handler whose outcome uses a `$`-reserved custom code, so building its res bytes fails. */
class ReservedCodeHandler extends ParleyCommandHandler<unknown, unknown, unknown, string> {
  #pending = new Map<string, (o: ParleyOutcome<unknown, string>) => void>();

  constructor(transport: FakeTransport, clock: VirtualClock) {
    super(transport, topics(), {}, { clock });
  }

  protected override handleCommand(
    _request: unknown,
    execution: ParleyExecution<unknown>,
  ): Promise<ParleyOutcome<unknown, string>> {
    return new Promise((resolve) => this.#pending.set(execution.commandId, resolve));
  }

  failReserved(id: string): void {
    // Deliberately construct an outcome with a $-reserved code; the outcome value is legal to make,
    // but serializing it must fail-fast and abort the command like a thrown handler.
    this.#pending.get(id)!(ParleyOutcome.error('$sneaky'));
  }
}

describe('outcome $-prefix fail-fast (§5)', () => {
  test('a $-reserved custom error aborts the command like a thrown handler (never a leak)', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new ReservedCodeHandler(transport, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A'));
    expect(handler._commandCount).toBe(1);
    expect(handler._executingCount).toBe(1);

    transport.clearOutbound();
    handler.failReserved('A');
    await drainMicrotasks();

    expect(handler._commandCount).toBe(0); // slot freed
    expect(handler._executingCount).toBe(0); // heartbeat stopped, execution slot released
    expect(handler._tombstoneCount).toBe(1); // id tombstoned
    expect(handler._liveTimerCount).toBe(1); // only the tombstone TTL timer remains
    expect(transport.publishCount('result', 'res')).toBe(0); // no result sent

    clock.advanceTo(61_000); // drain the tombstone
    expect(handler._tombstoneCount).toBe(0);
    expect(handler._liveTimerCount).toBe(0);
  });
});
