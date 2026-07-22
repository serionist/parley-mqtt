import { describe, expect, test } from 'vitest';
import { ParleyCommander, ParleyTopics, type ParleyResult } from '../src/index';
import { FakeTransport } from './support/fakeTransport';
import { ScriptedIdGenerator } from './support/scriptedIdGenerator';
import { VirtualClock } from './support/virtualClock';
import { VectorHandler } from './support/vectorHandler';
import { wire } from './support/wire';

const topics = (): ParleyTopics => new ParleyTopics('request', 'heartbeat', 'progress', 'result');

function newCommander(): {
  transport: FakeTransport;
  clock: VirtualClock;
  gen: ScriptedIdGenerator;
  commander: ParleyCommander<unknown, unknown, unknown, string>;
} {
  const clock = new VirtualClock();
  const transport = new FakeTransport();
  const gen = new ScriptedIdGenerator();
  const commander = new ParleyCommander<unknown, unknown, unknown, string>(transport, topics(), {}, {
    idGenerator: gen,
    clock,
  });
  transport.clearOutbound();
  return { transport, clock, gen, commander };
}

function expectError(result: ParleyResult<unknown, string>, code: string): void {
  expect(result.ok).toBe(false);
  if (!result.ok) {
    expect(result.error.code).toBe(code);
  }
}

describe('cancellation (C7/H7)', () => {
  test('token cancellation sends cnl, stops req retries, completes $cancelled', async () => {
    const { transport, clock, gen, commander } = newCommander();
    gen.enqueue('A');
    const controller = new AbortController();
    const p = commander.execute({}, { signal: controller.signal });
    expect(transport.publishCount('request', 'req')).toBe(1);

    transport.clearOutbound();
    controller.abort();
    expect(transport.publishCount('request', 'cnl')).toBe(1); // cnl published immediately

    transport.clearOutbound();
    clock.advanceTo(2000);
    expect(transport.publishCount('request', 'req')).toBe(0); // req retries stopped in SENDING

    transport.deliver('result', wire.resultError('A', '$cancelled'));
    expectError(await p, '$cancelled');
  });

  test('execute with an already-aborted signal publishes cnl promptly (no deadlock)', async () => {
    const { transport, gen, commander } = newCommander();
    gen.enqueue('A');
    const controller = new AbortController();
    controller.abort();

    const p = commander.execute({}, { signal: controller.signal });
    expect(transport.publishCount('request', 'cnl')).toBe(1);

    transport.deliver('result', wire.resultError('A', '$cancelled'));
    expectError(await p, '$cancelled');
  });

  test('cancel after successful completion is a no-op', async () => {
    const { transport, gen, commander } = newCommander();
    gen.enqueue('A');
    const controller = new AbortController();
    const p = commander.execute({}, { signal: controller.signal });
    transport.deliver('heartbeat', wire.heartbeat('A'));
    transport.deliver('result', wire.resultOk('A', '{"r":1}'));
    const result = await p;
    expect(result.ok).toBe(true);

    transport.clearOutbound();
    controller.abort(); // registration released on teardown -> no-op

    expect(transport.publishCount('request', 'cnl')).toBe(0);
  });

  test('handler cancel-executing signals the abort signal', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(transport, topics(), {}, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.request('A'));
    expect(handler.isExecuting('A')).toBe(true);
    expect(handler.isCancelRequested('A')).toBe(false);

    transport.deliver('request', wire.cancel('A'));
    expect(handler.isCancelRequested('A')).toBe(true); // H7
  });

  test('handler cancel of an unknown id is stateless $cancelled', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    const handler = new VectorHandler(transport, topics(), {}, clock);
    transport.clearOutbound();

    transport.deliver('request', wire.cancel('ghost'));

    const res = transport.publishes('result');
    expect(res).toHaveLength(1);
    expect(res[0]!.id).toBe('ghost');
    expect(res[0]!.e).toBe('$cancelled');
    expect(handler._commandCount).toBe(0);
    expect(handler._tombstoneCount).toBe(0);
    expect(handler._liveTimerCount).toBe(0);
  });
});

describe('reconnect resubscription (§2)', () => {
  test('commander connected resubscribes all three handler topics', () => {
    const { transport } = newCommander();
    transport.fireConnected();

    expect(transport.subscribeCount('heartbeat')).toBe(1);
    expect(transport.subscribeCount('progress')).toBe(1);
    expect(transport.subscribeCount('result')).toBe(1);
    expect(transport.subscribeCount('request')).toBe(0);
  });

  test('handler connected resubscribes the request topic', () => {
    const clock = new VirtualClock();
    const transport = new FakeTransport();
    new VectorHandler(transport, topics(), {}, clock);
    transport.clearOutbound();

    transport.fireConnected();

    expect(transport.subscribeCount('request')).toBe(1);
    expect(transport.subscribeCount('heartbeat')).toBe(0);
    expect(transport.subscribeCount('progress')).toBe(0);
    expect(transport.subscribeCount('result')).toBe(0);
  });
});
