/**
 * Parley layer-3 interop test peer (JS/TS), a GENERIC interpreter of
 * docs/conformance/interop/scenarios.json over the real mqtt.js client. Handler role serves the five
 * fixed harness command behaviours and prints READY after all SUBACKs; commander role runs the step
 * model, writes the JSON report, and exits nonzero on any failure. Zero suite logic — only the harness.
 *
 * The Parley core is consumed from source (the frozen js/src tree, zero runtime deps); tsx transpiles
 * the whole tree at run time. QoS 0, retain=false (protocol.md §2 baseline).
 */

import { readFileSync, writeFileSync } from 'node:fs';
import process from 'node:process';

import mqtt, { type MqttClient } from 'mqtt';

import {
  ParleyCommander,
  ParleyCommandHandler,
  ParleyConnectionAdapter,
  ParleyOutcome,
  ParleyTopics,
  type ParleyConfig,
  type ParleyExecution,
  type ParleyMessageHandler,
  type ParleyOutcome as ParleyOutcomeType,
  type ParleyResult,
} from '../src/index';

const SELF_LANG = 'js';
const READY_LINE = 'PARLEY-PEER-READY';
const COMMANDS = ['echo', 'slow', 'err', 'cancelme', 'busy1'] as const;

// ---------------------------------------------------------------------------
// Real transport adapter over mqtt.js, with SUBACK tracking so READY / suite
// start gate on the broker actually holding the subscriptions (QoS 0 has no
// retained state — publishing before a subscription lands would drop silently).
// ---------------------------------------------------------------------------
// mqtt.js auto-reconnects (reconnectPeriod default) and refires its `connect` event on every
// reconnection; the adapter forwards that to raiseConnected() so the Parley core re-subscribes. The
// client is created with `resubscribe: false` (see main) so the LIBRARY is the sole resubscriber — a
// double subscribe would be harmless (replace-semantics) but keeping one owner is cleaner.
class MqttJsAdapter extends ParleyConnectionAdapter {
  readonly #client: MqttClient;
  readonly #qos: 0 | 1;
  readonly #handlers = new Map<string, ParleyMessageHandler>();
  readonly #pending: Array<Promise<void>> = [];

  constructor(client: MqttClient, qos: 0 | 1) {
    super();
    this.#client = client;
    this.#qos = qos;
    this.#client.on('message', (topic: string, payload: Buffer) => {
      this.#handlers.get(topic)?.(topic, payload);
    });
    this.#client.on('connect', () => this.raiseConnected());
  }

  override publish(topic: string, payload: Uint8Array): void {
    this.#client.publish(topic, Buffer.from(payload), { qos: this.#qos, retain: false });
  }

  override subscribe(topic: string, onMessage: ParleyMessageHandler): void {
    this.#handlers.set(topic, onMessage);
    if (this.#client.connected) {
      this.#pending.push(
        new Promise<void>((resolve) => {
          this.#client.subscribe(topic, { qos: this.#qos }, () => resolve());
        }),
      );
    }
  }

  override unsubscribe(topic: string): void {
    this.#handlers.delete(topic);
    if (this.#client.connected) {
      this.#client.unsubscribe(topic);
    }
  }

  async waitForSubscriptions(): Promise<void> {
    await Promise.all(this.#pending);
  }
}

// ---------------------------------------------------------------------------
// The harness handler — the five fixed command behaviours.
// ---------------------------------------------------------------------------
class HarnessHandler extends ParleyCommandHandler<unknown, unknown, unknown, string> {
  readonly #kind: string;

  constructor(adapter: ParleyConnectionAdapter, topics: ParleyTopics, config: ParleyConfig, kind: string) {
    super(adapter, topics, config);
    this.#kind = kind;
  }

  protected override async handleCommand(
    request: unknown,
    execution: ParleyExecution<unknown>,
  ): Promise<ParleyOutcomeType<unknown, string>> {
    switch (this.#kind) {
      case 'echo':
        return ParleyOutcome.success(request);

      case 'err':
        return ParleyOutcome.error<string>(strField(request, 'code', 'error'));

      case 'slow': {
        const durationMs = numField(request, 'durationMs', 1000);
        const everyMs = Math.max(1, numField(request, 'progressEveryMs', 200));
        execution.reportProgress({ i: 0 });
        let elapsed = 0;
        let i = 0;
        while (elapsed < durationMs) {
          await delay(everyMs);
          elapsed += everyMs;
          execution.reportProgress({ i: ++i });
        }
        return ParleyOutcome.success({ done: true });
      }

      case 'cancelme':
      case 'busy1':
      default: {
        const everyMs = Math.max(1, numField(request, 'progressEveryMs', 200));
        execution.reportProgress({ i: 0 });
        let i = 0;
        while (!execution.signal.aborted) {
          await delay(everyMs);
          if (execution.signal.aborted) {
            break;
          }
          execution.reportProgress({ i: ++i });
        }
        return ParleyOutcome.cancelled();
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The commander step interpreter.
// ---------------------------------------------------------------------------
interface CallRecord {
  command: string;
  controller: AbortController;
  progress: number;
  done: boolean;
  result?: ParleyResult<unknown, string>;
  promise: Promise<ParleyResult<unknown, string>>;
}

class PeerAssert extends Error {}

interface StepResult {
  scenario: string;
  pass: boolean;
  detail: string;
}

type Commanders = Map<string, ParleyCommander<unknown, unknown, unknown, string>>;

async function runScenario(
  scenario: Record<string, unknown>,
  commanders: Commanders,
  barrierMs: number,
  callMs: number,
): Promise<StepResult> {
  const name = String(scenario.name);
  const named = new Map<string, CallRecord>();
  try {
    for (const rawStep of scenario.steps as Array<Record<string, unknown>>) {
      if (rawStep.call) {
        const call = rawStep.call as Record<string, unknown>;
        const rec = startCall(call, commanders);
        const res = await awaitCap(rec, callMs);
        checkExpect(name, call.expect as Record<string, unknown>, res, rec);
      } else if (rawStep.callAsync) {
        const call = rawStep.callAsync as Record<string, unknown>;
        named.set(String(call.name), startCall(call, commanders));
      } else if (rawStep.awaitProgress) {
        const ref = rawStep.awaitProgress as Record<string, unknown>;
        await barrier(name, requireCall(named, ref), barrierMs);
      } else if (rawStep.cancel) {
        const ref = rawStep.cancel as Record<string, unknown>;
        requireCall(named, ref).controller.abort();
      } else if (rawStep.barrier) {
        // Driver rendezvous: emit a flushed PARLEY-BARRIER line and continue. The driver watches the
        // log stream for this and performs the container action (kill handler / restart broker).
        const b = rawStep.barrier as Record<string, unknown>;
        const label = typeof b.label === 'string' ? b.label : name;
        process.stdout.write(`PARLEY-BARRIER ${label}\n`);
      } else if (rawStep.await) {
        const ref = rawStep.await as Record<string, unknown>;
        const rec = requireCall(named, ref);
        const res = await awaitCap(rec, callMs);
        checkExpect(name, ref.expect as Record<string, unknown>, res, rec);
      } else {
        throw new PeerAssert(`${name}: unrecognized step ${JSON.stringify(rawStep)}`);
      }
    }
    return { scenario: name, pass: true, detail: '' };
  } catch (e) {
    const detail = e instanceof PeerAssert ? e.message : `unexpected: ${String(e)}`;
    return { scenario: name, pass: false, detail };
  }
}

function startCall(call: Record<string, unknown>, commanders: Commanders): CallRecord {
  const command = String(call.command);
  const commander = commanders.get(command);
  if (!commander) {
    throw new PeerAssert(`unknown command '${command}'`);
  }
  const controller = new AbortController();
  const rec: CallRecord = {
    command,
    controller,
    progress: 0,
    done: false,
    promise: undefined as unknown as Promise<ParleyResult<unknown, string>>,
  };
  rec.promise = commander.execute(call.p, {
    onProgress: () => {
      rec.progress += 1;
    },
    signal: controller.signal,
  });
  rec.promise.then(
    (r) => {
      rec.done = true;
      rec.result = r;
    },
    () => {
      rec.done = true;
    },
  );
  return rec;
}

function requireCall(named: Map<string, CallRecord>, ref: Record<string, unknown>): CallRecord {
  const rec = named.get(String(ref.name));
  if (!rec) {
    throw new PeerAssert(`reference to unknown call '${String(ref.name)}'`);
  }
  return rec;
}

async function barrier(scenario: string, rec: CallRecord, capMs: number): Promise<void> {
  const start = Date.now();
  while (Date.now() - start < capMs) {
    if (rec.progress >= 1) {
      return;
    }
    if (rec.done) {
      throw new PeerAssert(`${scenario}: awaitProgress '${rec.command}' — terminal outcome before any progress`);
    }
    await delay(15);
  }
  throw new PeerAssert(`${scenario}: awaitProgress '${rec.command}' — no progress within ${capMs}ms`);
}

async function awaitCap(rec: CallRecord, capMs: number): Promise<ParleyResult<unknown, string>> {
  const winner = await Promise.race([
    rec.promise.then(() => 'done' as const),
    delay(capMs).then(() => 'timeout' as const),
  ]);
  if (winner === 'timeout') {
    throw new PeerAssert(`call '${rec.command}' — no terminal outcome within ${capMs}ms`);
  }
  return rec.result as ParleyResult<unknown, string>;
}

function checkExpect(
  scenario: string,
  expect: Record<string, unknown>,
  res: ParleyResult<unknown, string>,
  rec: CallRecord,
): void {
  if (expect.ok) {
    if (!res.ok) {
      throw new PeerAssert(`${scenario}: expected success, got error '${res.error.code}'`);
    }
    if ('p' in expect) {
      if (!deepEqual(res.value, expect.p)) {
        throw new PeerAssert(
          `${scenario}: result payload mismatch. expected ${JSON.stringify(expect.p)}, got ${JSON.stringify(res.value)}`,
        );
      }
    }
  } else {
    if (res.ok) {
      throw new PeerAssert(`${scenario}: expected error but succeeded`);
    }
    if ('e' in expect && res.error.code !== expect.e) {
      throw new PeerAssert(`${scenario}: error code mismatch. expected '${String(expect.e)}', got '${res.error.code}'`);
    }
  }
  if ('minProgress' in expect && rec.progress < Number(expect.minProgress)) {
    throw new PeerAssert(`${scenario}: minProgress not met. expected >= ${Number(expect.minProgress)}, got ${rec.progress}`);
  }
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
function deepEqual(a: unknown, b: unknown): boolean {
  if (a === b) {
    return true;
  }
  if (a === null || b === null) {
    return a === b;
  }
  const ta = typeof a;
  const tb = typeof b;
  if (ta !== tb) {
    return false;
  }
  if (ta !== 'object') {
    return a === b; // number / string / boolean (bool !== number since typeof differs)
  }
  const aArr = Array.isArray(a);
  const bArr = Array.isArray(b);
  if (aArr !== bArr) {
    return false;
  }
  if (aArr) {
    const av = a as unknown[];
    const bv = b as unknown[];
    if (av.length !== bv.length) {
      return false;
    }
    for (let i = 0; i < av.length; i++) {
      if (!deepEqual(av[i], bv[i])) {
        return false;
      }
    }
    return true;
  }
  const ao = a as Record<string, unknown>;
  const bo = b as Record<string, unknown>;
  const ka = Object.keys(ao);
  const kb = Object.keys(bo);
  if (ka.length !== kb.length) {
    return false;
  }
  for (const k of ka) {
    if (!Object.prototype.hasOwnProperty.call(bo, k) || !deepEqual(ao[k], bo[k])) {
      return false;
    }
  }
  return true;
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function field(request: unknown, key: string): unknown {
  return request !== null && typeof request === 'object' ? (request as Record<string, unknown>)[key] : undefined;
}

function numField(request: unknown, key: string, def: number): number {
  const v = field(request, key);
  return typeof v === 'number' ? v : def;
}

function strField(request: unknown, key: string, def: string): string {
  const v = field(request, key);
  return typeof v === 'string' ? v : def;
}

function topics(prefix: string, cmd: string): ParleyTopics {
  return new ParleyTopics(`${prefix}/${cmd}/req`, `${prefix}/${cmd}/hb`, `${prefix}/${cmd}/prg`, `${prefix}/${cmd}/res`);
}

function buildConfig(c: Record<string, unknown>, maxActiveCommands: number): ParleyConfig {
  return {
    requestRetryInterval: c.requestRetryInterval as number,
    noAckTimeout: c.noAckTimeout as number,
    staleTimeout: c.staleTimeout as number,
    executionTimeout: c.executionTimeout as number,
    cancelRetryInterval: c.cancelRetryInterval as number,
    heartbeatInterval: c.heartbeatInterval as number,
    resultRetryInterval: c.resultRetryInterval as number,
    resultRetryTimeout: c.resultRetryTimeout as number,
    tombstoneTtl: c.tombstoneTtl as number,
    maxConcurrentExecutions: c.maxConcurrentExecutions as number,
    maxActiveCommands,
  };
}

function parseArgs(argv: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  for (let i = 0; i < argv.length; i++) {
    if (argv[i].startsWith('--') && i + 1 < argv.length) {
      out[argv[i].slice(2)] = argv[i + 1];
      i++;
    }
  }
  return out;
}

function parseBroker(broker: string): { host: string; port: number } {
  let s = broker;
  const scheme = s.indexOf('://');
  if (scheme >= 0) {
    s = s.slice(scheme + 3);
  }
  const colon = s.lastIndexOf(':');
  if (colon < 0) {
    return { host: s, port: 1883 };
  }
  return { host: s.slice(0, colon), port: Number(s.slice(colon + 1)) };
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
async function main(): Promise<void> {
  const opt = parseArgs(process.argv.slice(2));
  const role = opt.role;
  const { host, port } = parseBroker(opt.broker);
  const prefix = opt.prefix;
  const scenariosPath = opt.scenarios ?? 'scenarios.json';
  const suiteKey = opt.suite === 'selfchecks' ? 'selfChecks' : 'scenarios';
  const handlerLang = opt['handler-lang'] ?? '-';
  const scenarioFilter = opt.scenario;
  const qos: 0 | 1 = opt.qos === '1' ? 1 : 0;

  const root = JSON.parse(readFileSync(scenariosPath, 'utf8')) as Record<string, unknown>;
  const cfg = root.config as Record<string, unknown>;
  const baseMaxActive = (cfg.maxActiveCommands as number) ?? 8;
  const timeouts = root.timeouts as Record<string, number>;
  const barrierMs = timeouts.awaitProgressMs;
  const callMs = timeouts.callMs;

  // resubscribe:false — the Parley core is the sole resubscriber via the connect-event → raiseConnected
  // path (mqtt.js auto-reconnects and refires `connect` on every reconnection).
  const client = mqtt.connect(`mqtt://${host}:${port}`, { protocolVersion: 4, clean: true, resubscribe: false });
  await new Promise<void>((resolve, reject) => {
    client.once('connect', () => resolve());
    client.once('error', reject);
  });
  const adapter = new MqttJsAdapter(client, qos);

  if (role === 'handler') {
    const handlers: HarnessHandler[] = [];
    for (const cmd of COMMANDS) {
      const maxActive = cmd === 'busy1' ? 1 : baseMaxActive;
      handlers.push(new HarnessHandler(adapter, topics(prefix, cmd), buildConfig(cfg, maxActive), cmd));
    }
    await adapter.waitForSubscriptions();
    console.log(READY_LINE);
    await new Promise<void>(() => {
      /* serve until killed */
    });
    return;
  }

  const commanders: Commanders = new Map();
  for (const cmd of COMMANDS) {
    commanders.set(cmd, new ParleyCommander(adapter, topics(prefix, cmd), buildConfig(cfg, baseMaxActive)));
  }
  await adapter.waitForSubscriptions();

  const results: StepResult[] = [];
  for (const scenario of root[suiteKey] as Array<Record<string, unknown>>) {
    if (scenarioFilter && String(scenario.name) !== scenarioFilter) {
      continue;
    }
    results.push(await runScenario(scenario, commanders, barrierMs, callMs));
  }
  if (scenarioFilter && results.length === 0) {
    results.push({ scenario: scenarioFilter, pass: false, detail: `no scenario named '${scenarioFilter}' in suite '${suiteKey}'` });
  }

  const allPass = results.every((r) => r.pass);
  const report = JSON.stringify(
    { commander: SELF_LANG, handler: handlerLang, prefix, suite: suiteKey, results, pass: allPass },
    null,
    2,
  );
  console.log(report);
  if (opt.report) {
    writeFileSync(opt.report, report);
  }

  for (const c of commanders.values()) {
    c.dispose();
  }
  client.end();
  process.exit(allPass ? 0 : 1);
}

void main();
