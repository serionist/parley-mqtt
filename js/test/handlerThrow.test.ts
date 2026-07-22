import { describe, expect, test } from 'vitest';
import {
  ParleyCommandHandler,
  ParleyTopics,
  type ParleyExecution,
  type ParleyOutcome,
} from '../src/index';
import { drainMicrotasks } from './support/drain';
import { FakeTransport } from './support/fakeTransport';
import { VirtualClock } from './support/virtualClock';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

/** A handler whose {@link handleCommand} parks on a promise the test rejects (an unwinding throw). */
class ThrowingHandler extends ParleyCommandHandler<unknown, unknown, unknown, string> {
  readonly #pending = new Map<string, { reject: (e: unknown) => void; execution: ParleyExecution<unknown> }>();

  constructor(transport: FakeTransport, clock: VirtualClock) {
    super(transport, topics(), {}, { clock });
  }

  protected override handleCommand(
    _request: unknown,
    execution: ParleyExecution<unknown>,
  ): Promise<ParleyOutcome<unknown, string>> {
    return new Promise<ParleyOutcome<unknown, string>>((_resolve, reject) => {
      this.#pending.set(execution.commandId, { reject, execution });
    });
  }

  /** Makes the parked handleCommand throw (reject), like an `AbortError`/exception unwinding the body. */
  throwNow(id: string): void {
    this.#pending.get(id)!.reject(new Error('handler unwound via a throw'));
  }

  isCancelRequested(id: string): boolean {
    return this.#pending.get(id)!.execution.signal.aborted;
  }
}

describe('thrown cancellation maps to $cancelled (HC1)', () => {
  test('a throw while the execution signal is aborted yields a cached, retried $cancelled', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new ThrowingHandler(transport, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A')); // admit → executing
    transport.deliver('request', wire.cancel('A')); // H7 → the execution signal is aborted
    expect(handler.isCancelRequested('A')).toBe(true);

    transport.clearOutbound();
    handler.throwNow('A'); // handler unwinds via a throw WHILE aborted
    await drainMicrotasks();

    // HC1: routed through normal completion — a $cancelled res is published and cached/retried (H5),
    // NOT a silent abort.
    const res = transport.publishes('result');
    expect(res).toHaveLength(1);
    expect(res[0]!.id).toBe('A');
    expect(res[0]!.ok).toBe(false);
    expect(res[0]!.e).toBe('$cancelled');

    // It lives in RESULT-CACHED (slot held) with the execution slot released.
    expect(handler._commandCount).toBe(1);
    expect(handler._executingCount).toBe(0);

    // A rak tombstones it and drains state — proving it went through the cached/retried path.
    transport.deliver('request', wire.resultAck('A'));
    expect(handler._commandCount).toBe(0);
    expect(handler._executingCount).toBe(0);
    expect(handler._tombstoneCount).toBe(1);
  });

  test('a throw WITHOUT the signal aborted aborts silently (no result published)', async () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new ThrowingHandler(transport, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A'));
    expect(handler.isCancelRequested('A')).toBe(false);

    transport.clearOutbound();
    handler.throwNow('A'); // a genuine programming error, signal not aborted
    await drainMicrotasks();

    // Non-cancellation throw → abort: slot freed, id tombstoned, and NO result reaches the wire.
    expect(transport.publishCount('result', 'res')).toBe(0);
    expect(handler._commandCount).toBe(0);
    expect(handler._executingCount).toBe(0);
    expect(handler._tombstoneCount).toBe(1);
    expect(handler._liveTimerCount).toBe(1); // only the tombstone TTL timer
  });
});
