import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import {
  ParleyCommander,
  ParleyTopics,
  type ParleyCallOptions,
  type ParleyConfig,
  type ParleyResult,
} from '../../src/index';
import { drainMicrotasks } from './drain';
import { FakeTransport, type OutboundAction } from './fakeTransport';
import { jsonEqual } from './jsonEqual';
import { ScriptedIdGenerator } from './scriptedIdGenerator';
import { VirtualClock } from './virtualClock';
import { VectorHandler } from './vectorHandler';

// The topic labels the vectors use double as the actual (opaque) topic strings.
const REQUEST = 'request';
const HEARTBEAT = 'heartbeat';
const PROGRESS = 'progress';
const RESULT = 'result';

const decoder = new TextDecoder();
const encoder = new TextEncoder();

const DIRECTIVE_KEYS = ['call', 'cancel', 'in', 'handler', 'connected', 'expect'] as const;

interface Step {
  at: number;
  call?: { id: string; p?: unknown; options?: Record<string, unknown> };
  cancel?: string;
  in?: { topic: string; msg?: unknown; raw?: string };
  handler?: { id: string; action: string; p?: unknown; e?: string; m?: string };
  connected?: boolean;
  expect?: ExpectItem[];
}

type ExpectItem = { topic: string; msg: unknown } | { subscribe: string };

interface Vector {
  name?: string;
  role: 'commander' | 'handler';
  config?: ParleyConfig;
  steps: Step[];
  outcomes?: Record<string, { ok: boolean; p?: unknown; e?: string }>;
}

interface OutcomeRecord {
  settled: boolean;
  value?: ParleyResult<unknown, string>;
  error?: unknown;
}

export async function runVector(path: string): Promise<void> {
  const vector = JSON.parse(readFileSync(path, 'utf8')) as Vector;
  const name = vector.name ?? basename(path);
  const config = vector.config ?? {};
  const clock = new VirtualClock();
  const transport = new FakeTransport();
  const topics = new ParleyTopics(REQUEST, HEARTBEAT, PROGRESS, RESULT);

  if (vector.role === 'commander') {
    await runCommander(name, config, vector.steps, vector.outcomes, clock, transport, topics);
  } else if (vector.role === 'handler') {
    await runHandler(name, config, vector.steps, clock, transport, topics);
  } else {
    throw new Error(`Vector '${name}': unknown role '${String(vector.role)}'.`);
  }
}

async function runCommander(
  name: string,
  config: ParleyConfig,
  steps: Step[],
  outcomes: Vector['outcomes'],
  clock: VirtualClock,
  transport: FakeTransport,
  topics: ParleyTopics,
): Promise<void> {
  const idGenerator = new ScriptedIdGenerator();
  const commander = new ParleyCommander<unknown, unknown, unknown, string>(transport, topics, config, {
    idGenerator,
    clock,
  });

  // Construction-time subscribes are not asserted (protocol.md §10).
  transport.clearOutbound();

  const records = new Map<string, OutcomeRecord>();
  const controllers = new Map<string, AbortController>();
  let lastAt = 0;

  for (const step of steps) {
    enforceSingleDirective(name, step);
    assertValidAt(name, step);
    assertNonDecreasing(name, step.at, lastAt);
    lastAt = step.at;

    clock.advanceTo(step.at);
    await drainMicrotasks();

    if (step.call) {
      const id = step.call.id;
      idGenerator.enqueue(id);
      const controller = new AbortController();
      controllers.set(id, controller);
      const record: OutcomeRecord = { settled: false };
      records.set(id, record);
      const options: ParleyCallOptions & { signal: AbortSignal } = {
        ...((step.call.options ?? {}) as ParleyCallOptions),
        signal: controller.signal,
      };
      void commander.execute(step.call.p, options).then(
        (value) => {
          record.settled = true;
          record.value = value;
        },
        (error: unknown) => {
          record.settled = true;
          record.error = error;
        },
      );
    } else if (step.cancel !== undefined) {
      const controller = controllers.get(step.cancel);
      if (!controller) {
        throw new Error(`Vector '${name}': cancel for unknown call '${step.cancel}'.`);
      }
      controller.abort();
    } else if (step.in) {
      deliverInbound(transport, step.in);
    } else if (step.connected !== undefined) {
      transport.fireConnected();
    } else if (step.expect) {
      assertExpect(name, step.at, transport.outbound, step.expect);
      transport.clearOutbound();
    } else {
      throw new Error(`Vector '${name}': unrecognized step ${JSON.stringify(step)}.`);
    }

    await drainMicrotasks();
  }

  // TQ1: every outbound action must have been asserted by an expect — nothing left unaccounted for.
  assertOutboxEmpty(name, transport);
  await drainMicrotasks();
  assertOutcomes(name, outcomes, records);
}

async function runHandler(
  name: string,
  config: ParleyConfig,
  steps: Step[],
  clock: VirtualClock,
  transport: FakeTransport,
  topics: ParleyTopics,
): Promise<void> {
  const handler = new VectorHandler(transport, topics, config, clock);

  // Construction-time subscribes are not asserted (protocol.md §10).
  transport.clearOutbound();

  let lastAt = 0;

  for (const step of steps) {
    enforceSingleDirective(name, step);
    assertValidAt(name, step);
    assertNonDecreasing(name, step.at, lastAt);
    lastAt = step.at;

    clock.advanceTo(step.at);
    await drainMicrotasks();

    if (step.in) {
      deliverInbound(transport, step.in);
    } else if (step.handler) {
      applyHandlerAction(name, handler, step.handler);
    } else if (step.connected !== undefined) {
      transport.fireConnected();
    } else if (step.expect) {
      assertExpect(name, step.at, transport.outbound, step.expect);
      transport.clearOutbound();
    } else {
      throw new Error(`Vector '${name}': unrecognized step ${JSON.stringify(step)}.`);
    }

    await drainMicrotasks();
  }

  assertOutboxEmpty(name, transport);
}

function enforceSingleDirective(name: string, step: Step): void {
  let count = 0;
  for (const key of DIRECTIVE_KEYS) {
    if (Object.prototype.hasOwnProperty.call(step, key)) {
      count++;
    }
  }
  if (count !== 1) {
    throw new Error(
      `Vector '${name}': each step must carry exactly one directive, found ${String(count)}: ${JSON.stringify(step)}`,
    );
  }
}

function assertValidAt(name: string, step: Step): void {
  // TQ2: every step must carry a finite numeric `at` — mirrors the C# runner, which throws rather
  // than silently treating a missing/NaN time as 0 (which would mis-order the whole vector).
  if (typeof step.at !== 'number' || !Number.isFinite(step.at)) {
    throw new Error(`Vector '${name}': step 'at' must be a finite number, got ${JSON.stringify(step.at)}: ${JSON.stringify(step)}`);
  }
}

function assertNonDecreasing(name: string, at: number, lastAt: number): void {
  if (at < lastAt) {
    throw new Error(`Vector '${name}': step times must be non-decreasing (${String(at)} < ${String(lastAt)}).`);
  }
}

function deliverInbound(transport: FakeTransport, inbound: NonNullable<Step['in']>): void {
  const bytes =
    inbound.raw !== undefined ? encoder.encode(inbound.raw) : encoder.encode(JSON.stringify(inbound.msg));
  transport.deliver(inbound.topic, bytes);
}

function applyHandlerAction(
  name: string,
  handler: VectorHandler,
  action: NonNullable<Step['handler']>,
): void {
  switch (action.action) {
    case 'complete':
      handler.complete(action.id, action.p);
      break;
    case 'fail':
      handler.fail(action.id, action.e!, action.m);
      break;
    case 'progress':
      handler.emitProgress(action.id, action.p);
      break;
    default:
      throw new Error(`Vector '${name}': unknown handler action '${action.action}'.`);
  }
}

function assertExpect(
  name: string,
  at: number,
  actual: OutboundAction[],
  expected: ExpectItem[],
): void {
  const unmatched = [...actual];

  for (const item of expected) {
    let index: number;
    if ('subscribe' in item) {
      index = unmatched.findIndex((a) => a.kind === 'subscribe' && a.topic === item.subscribe);
    } else {
      index = unmatched.findIndex(
        (a) => a.kind === 'publish' && a.topic === item.topic && jsonEqual(parsePayload(a.payload), item.msg),
      );
    }

    if (index < 0) {
      throw new Error(
        `Vector '${name}' at t=${String(at)}: expected outbound action not found: ${JSON.stringify(item)}\nActual: ${dump(actual)}`,
      );
    }
    unmatched.splice(index, 1);
  }

  if (unmatched.length > 0) {
    throw new Error(
      `Vector '${name}' at t=${String(at)}: unexpected extra outbound action(s): ${dump(unmatched)}\nExpected: ${JSON.stringify(expected)}`,
    );
  }
}

function assertOutboxEmpty(name: string, transport: FakeTransport): void {
  if (transport.outbound.length > 0) {
    throw new Error(
      `Vector '${name}': ${String(transport.outbound.length)} outbound action(s) left unasserted after the final step: ${dump(transport.outbound)}`,
    );
  }
}

function assertOutcomes(
  name: string,
  outcomes: Vector['outcomes'],
  records: Map<string, OutcomeRecord>,
): void {
  // TQ2(a): every started call must have a declared expected outcome — nothing silently unasserted.
  for (const startedId of records.keys()) {
    if (!outcomes || !Object.prototype.hasOwnProperty.call(outcomes, startedId)) {
      throw new Error(`Vector '${name}': call '${startedId}' was started but has no expected outcome declared.`);
    }
  }

  if (!outcomes) {
    return;
  }

  for (const [id, expected] of Object.entries(outcomes)) {
    const record = records.get(id);
    if (!record) {
      throw new Error(`Vector '${name}': outcome expected for call '${id}' that was never started.`);
    }
    if (record.error !== undefined) {
      const detail =
        record.error instanceof Error ? record.error.message : JSON.stringify(record.error);
      throw new Error(`Vector '${name}': call '${id}' rejected unexpectedly: ${detail}`);
    }
    if (!record.settled || !record.value) {
      throw new Error(`Vector '${name}': call '${id}' did not reach a terminal outcome.`);
    }

    const result = record.value;
    if (expected.ok) {
      if (!result.ok) {
        throw new Error(`Vector '${name}': call '${id}' expected success but got error '${result.error.code}'.`);
      }
      // TQ2(b): an absent expected payload must match an absent/null actual payload, not be skipped.
      if (!Object.prototype.hasOwnProperty.call(expected, 'p')) {
        if (result.value !== undefined && result.value !== null) {
          throw new Error(
            `Vector '${name}': call '${id}' expected absent/null payload but got ${JSON.stringify(result.value)}.`,
          );
        }
      } else if (!jsonEqual(result.value, expected.p)) {
        throw new Error(
          `Vector '${name}': call '${id}' payload mismatch. Expected ${JSON.stringify(expected.p)}, got ${JSON.stringify(result.value)}.`,
        );
      }
    } else {
      if (result.ok) {
        throw new Error(`Vector '${name}': call '${id}' expected error but succeeded.`);
      }
      if (result.error.code !== expected.e) {
        throw new Error(
          `Vector '${name}': call '${id}' error code mismatch. Expected '${String(expected.e)}', got '${result.error.code}'.`,
        );
      }
    }
  }
}

function parsePayload(payload: Uint8Array): unknown {
  return JSON.parse(decoder.decode(payload));
}

function dump(actions: OutboundAction[]): string {
  return `[${actions
    .map((a) =>
      a.kind === 'subscribe'
        ? `{subscribe: ${a.topic}}`
        : `{topic: ${a.topic}, msg: ${decoder.decode(a.payload)}}`,
    )
    .join(', ')}]`;
}
