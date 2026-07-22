/**
 * Parley example (TypeScript) — a `backup` command over MQTT.
 * =========================================================================
 *
 * One command type, two roles:
 *
 *   • the HANDLER runs on the device that owns the disk. It receives a backup
 *     request, streams progress while it works, and returns how many bytes it
 *     wrote — or a typed error (the disk filled up, the target was unreachable).
 *
 *   • the COMMANDER runs wherever you want to kick a backup off. It calls the
 *     command, watches progress, can cancel, and gets one typed answer back.
 *
 * Both sides talk over the MQTT broker you already have. Parley itself never
 * opens the connection — you hand it a live one through a small adapter (below).
 *
 * The command in numbers:
 *   request   { target: string }      — what to back up
 *   progress  { percent: number }     — advisory, streamed while working
 *   result    { bytesWritten: number} — the single terminal success value
 *   errors    "diskFull" | "targetUnreachable"  — your own typed error codes
 *
 * Run the two roles in separate processes (or on separate machines). Everything
 * below is a single file only so the whole picture fits on one screen.
 */

import mqtt, { type MqttClient } from 'mqtt';

import {
  ParleyBuiltinError,
  ParleyCommander,
  ParleyCommandHandler,
  ParleyConnectionAdapter,
  ParleyOutcome,
  ParleyTopics,
  type ParleyExecution,
  type ParleyMessageHandler,
  type ParleyOutcome as ParleyOutcomeType,
} from 'parley-mqtt';

// ---------------------------------------------------------------------------
// 1. The message shapes for this command.
// ---------------------------------------------------------------------------
// These are just your own types — Parley binds them with TypeScript generics;
// there is no schema compiler and no code generation. The four type parameters
// on a commander/handler are always <Request, Progress, Result, Error>.

interface BackupRequest {
  target: string;
}
interface BackupProgress {
  percent: number;
}
interface BackupResult {
  bytesWritten: number;
}

// The custom error codes this command can fail with. They are ordinary strings
// on the wire; the union type is what makes them type-checked in your code.
// (Codes must not start with `$` — that prefix is reserved for Parley's builtins.)
type BackupError = 'diskFull' | 'targetUnreachable';

// The four topic strings. Parley imposes no naming scheme — these are yours, and
// your broker's ACLs apply to them directly. Both roles must agree on the set.
const backupTopics = new ParleyTopics(
  'home/backup/req', // commander → handler: req, rak, cnl
  'home/backup/hb', //  handler → commander: heartbeats
  'home/backup/prg', // handler → commander: progress
  'home/backup/res', // handler → commander: the result
);

// ===========================================================================
// 2. An ILLUSTRATIVE transport adapter over mqtt.js.
// ===========================================================================
// ⚠️  This adapter is hand-written for the example. It has NOT been shipped as a
//     package or run through CI — it shows the shape you would implement over
//     your own client. A verified, reusable adapter arrives with the e2e suite.
//
// The contract is tiny (protocol.md §2): publish, subscribe, unsubscribe, and a
// "connected" signal. Parley never connects or holds credentials — the mqtt.js
// client below is fully connected and authenticated before Parley ever sees it.
class MqttJsAdapter extends ParleyConnectionAdapter {
  readonly #client: MqttClient;
  readonly #handlers = new Map<string, ParleyMessageHandler>();

  constructor(client: MqttClient) {
    super();
    this.#client = client;

    // Route every inbound MQTT message to the Parley handler registered for its
    // topic. mqtt.js hands us a Buffer, which already *is* a Uint8Array.
    this.#client.on('message', (topic: string, payload: Buffer) => {
      this.#handlers.get(topic)?.(topic, payload);
    });

    // On every (re)connect, tell Parley — it re-subscribes its topics in
    // response, which is how a dropped connection self-heals.
    this.#client.on('connect', () => this.raiseConnected());
  }

  // Fire-and-forget publish at QoS 0, retain=false (all Parley requires). A
  // production adapter might queue when offline; here we simply hand it off.
  override publish(topic: string, payload: Uint8Array): void {
    this.#client.publish(topic, Buffer.from(payload), { qos: 0, retain: false });
  }

  // Replace-semantics: one handler per topic. We record it and, if we are
  // already connected, subscribe at the broker immediately.
  override subscribe(topic: string, onMessage: ParleyMessageHandler): void {
    this.#handlers.set(topic, onMessage);
    if (this.#client.connected) {
      this.#client.subscribe(topic, { qos: 0 });
    }
  }

  override unsubscribe(topic: string): void {
    this.#handlers.delete(topic);
    if (this.#client.connected) {
      this.#client.unsubscribe(topic);
    }
  }
}

// ===========================================================================
// 3. The HANDLER — the side that performs the backup.
// ===========================================================================
// Subclass ParleyCommandHandler and implement handleCommand. Expected failures
// are RETURNED as ParleyOutcome values, never thrown — throwing is reserved for
// genuine bugs (which Parley contains: the command aborts, nothing leaks).
class BackupHandler extends ParleyCommandHandler<
  BackupRequest,
  BackupProgress,
  BackupResult,
  BackupError
> {
  protected override async handleCommand(
    request: BackupRequest,
    execution: ParleyExecution<BackupProgress>,
  ): Promise<ParleyOutcomeType<BackupResult, BackupError>> {
    // A made-up backup that "copies" ten chunks. While it runs, Parley is
    // heartbeating this command every few seconds on your behalf, so the caller
    // can always tell "slow" from "dead" — you never have to fake progress to
    // stay trusted.
    let bytesWritten = 0;

    for (let chunk = 1; chunk <= 10; chunk++) {
      // Cooperative cancellation: a wire-level `cnl` from the caller flips
      // execution.signal.aborted. We check it and stop cleanly.
      if (execution.signal.aborted) {
        return ParleyOutcome.cancelled(); // → the caller sees $cancelled
      }

      // Pretend to copy a chunk of the target.
      const wrote = await copyOneChunk(request.target, chunk);
      if (wrote < 0) {
        // Domain failures are typed values — the caller gets them back typed.
        return ParleyOutcome.error<BackupError>('diskFull', 'no space left on device');
      }
      bytesWritten += wrote;

      // Advisory progress. Each call publishes one `prg`; delivery is
      // best-effort (never acknowledged), so it is safe to send often.
      execution.reportProgress({ percent: chunk * 10 });
    }

    // The single terminal success value.
    return ParleyOutcome.success({ bytesWritten });
  }
}

// ===========================================================================
// 4. The COMMANDER — the side that requests a backup.
// ===========================================================================
async function requestBackup(commander: ParleyCommander<
  BackupRequest,
  BackupProgress,
  BackupResult,
  BackupError
>): Promise<void> {
  // A standard AbortController is a valid cancel signal. Abort it any time
  // before the result lands and Parley sends a real `cnl` to the handler.
  const controller = new AbortController();

  // Auto-cancel this backup if it somehow runs longer than five minutes.
  const giveUp = setTimeout(() => controller.abort(), 5 * 60_000);

  const result = await commander.execute(
    { target: 'photos' },
    {
      // Per-call options: watch progress, allow cancellation, and cap how long
      // we will wait once the handler is alive (protocol.md §8, milliseconds).
      onProgress: (p) => console.log(`backup ${p.percent}% done`),
      signal: controller.signal,
      executionTimeout: 5 * 60_000,
    },
  );

  clearTimeout(giveUp);

  // Every outcome — success, your errors, and all six builtins — is a value.
  if (result.ok) {
    console.log(`backup complete: ${result.value.bytesWritten} bytes written`);
    return;
  }

  const error = result.error;
  if (error.builtin) {
    // A builtin: something about delivery/liveness, not your domain.
    switch (error.builtin) {
      case ParleyBuiltinError.NoAck:
        console.error('no handler answered — is the device online?');
        break;
      case ParleyBuiltinError.Busy:
        console.error('the device is at capacity — retry shortly');
        break;
      case ParleyBuiltinError.ExecutionStale:
        console.error('the device went silent mid-backup');
        break;
      case ParleyBuiltinError.ExecutionTimeout:
        console.error('the backup ran past its time cap');
        break;
      case ParleyBuiltinError.Cancelled:
        console.error('the backup was cancelled');
        break;
      case ParleyBuiltinError.DecodeError:
        console.error('the device sent a result we could not read');
        break;
    }
  } else {
    // A custom code — typed as BackupError, so the compiler checks the cases.
    switch (error.custom) {
      case 'diskFull':
        console.error(`out of space: ${error.message ?? ''}`);
        break;
      case 'targetUnreachable':
        console.error('the backup target could not be reached');
        break;
    }
  }
}

// ===========================================================================
// 5. Wiring it up.
// ===========================================================================
async function main(): Promise<void> {
  const role = process.argv[2]; // "handler" or "commander"

  // YOU own the connection: url, credentials, and TLS all live here, never in
  // Parley. Wait until it is actually connected before doing work.
  const client = mqtt.connect('mqtt://localhost:1883', {
    // username / password / TLS options go here — Parley never sees them.
  });
  await new Promise<void>((resolve) => client.once('connect', () => resolve()));

  const adapter = new MqttJsAdapter(client);

  if (role === 'handler') {
    const handler = new BackupHandler(adapter, backupTopics);
    console.log('backup handler ready');
    // ... serve until shutdown ...
    process.on('SIGINT', () => {
      handler.dispose(); // detaches callbacks, cancels timers, frees all state
      client.end();
      process.exit(0);
    });
  } else {
    const commander = new ParleyCommander<
      BackupRequest,
      BackupProgress,
      BackupResult,
      BackupError
    >(adapter, backupTopics);
    await requestBackup(commander);
    commander.dispose(); // release the commander when you are done with it
    client.end();
  }
}

// A stand-in for real I/O. Returns bytes written, or -1 to simulate a full disk.
async function copyOneChunk(target: string, chunk: number): Promise<number> {
  await new Promise((r) => setTimeout(r, 200));
  return 1024 * chunk; // pretend each chunk is a little bigger
}

void main();
