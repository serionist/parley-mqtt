import type { ParleyConnectionAdapter } from './adapter';
import { SystemClock, type ParleyClock } from './clock';
import { resolveConfig, type ParleyConfig, type ParleyTopics, type ResolvedConfig } from './config';
import { ParleyBuiltinError, type ParleyBuiltinErrorCode, type ParleyOutcome } from './errors';
import {
  isValidId,
  MessageType,
  parse,
  PROTOCOL_VERSION,
  serialize,
  toPayload,
} from './envelope';
import {
  ParleyExecutionContext,
  ParleySignalSource,
  type ParleyExecution,
} from './execution';
import { ParleyRuntime, type ParleyTimer } from './runtime';
import { TombstoneTable } from './tombstones';

/** Optional per-handler codec hooks. All default to a raw pass-through. */
export interface ParleyHandlerCodec<TRequest, TResult, TProgress> {
  /** Decodes a `req` payload. Throwing drops the request silently — no admission (§4). Default: pass-through. */
  decodeRequest?(raw: unknown): TRequest;
  /** Encodes a success result payload. Default: pass-through. */
  encodeResult?(value: TResult): unknown;
  /** Encodes a progress payload. Default: pass-through. */
  encodeProgress?(value: TProgress): unknown;
}

/** Construction-time seams and codec for a {@link ParleyCommandHandler}. */
export interface ParleyHandlerOptions<TRequest, TResult, TProgress> {
  codec?: ParleyHandlerCodec<TRequest, TResult, TProgress>;
  clock?: ParleyClock;
}

type HandlerState = 'queued' | 'executing' | 'result-cached';

interface HandlerCommand<TRequest, TProgress> {
  readonly id: string;
  readonly request: TRequest;
  readonly abort: ParleySignalSource;
  readonly execution: ParleyExecutionContext<TProgress>;
  state: HandlerState;
  cachedHeartbeatBytes?: Uint8Array;
  cachedResultBytes?: Uint8Array;
  heartbeatTimer?: ParleyTimer;
  resultRetryTimer?: ParleyTimer;
  resultRetryTimeoutTimer?: ParleyTimer;
}

/**
 * The callee side of one command type (protocol.md §7, rules H1–H8). The consumer overrides
 * {@link handleCommand}. A topic set MUST have at most one active handler. Admission (dedup lookup,
 * capacity check, slot reservation, first heartbeat) is one atomic step — natural in single-threaded
 * JS because all state is mutated before any publish, so a synchronous re-entrant delivery observes
 * a consistent table and dedup catches the duplicate.
 */
export abstract class ParleyCommandHandler<TRequest, TProgress, TResult, TError = string> {
  readonly #adapter: ParleyConnectionAdapter;
  readonly #topics: ParleyTopics;
  readonly #config: ResolvedConfig;
  readonly #runtime: ParleyRuntime;
  readonly #tombstones: TombstoneTable;
  readonly #codec: ParleyHandlerCodec<TRequest, TResult, TProgress> | undefined;
  readonly #commands = new Map<string, HandlerCommand<TRequest, TProgress>>();
  readonly #queue: Array<HandlerCommand<TRequest, TProgress>> = [];
  readonly #onConnected = (): void => {
    if (this.#disposed) {
      return;
    }
    this.#subscribeAll();
  };
  #executingCount = 0;
  #dispatching = false;
  #disposed = false;

  constructor(
    adapter: ParleyConnectionAdapter,
    topics: ParleyTopics,
    config: ParleyConfig = {},
    options: ParleyHandlerOptions<TRequest, TResult, TProgress> = {},
  ) {
    this.#adapter = adapter;
    this.#topics = topics;
    this.#config = resolveConfig(config);
    this.#runtime = new ParleyRuntime(options.clock ?? new SystemClock());
    this.#tombstones = new TombstoneTable(this.#runtime, this.#config.maxTombstones);
    this.#codec = options.codec;

    this.#adapter.onConnected(this.#onConnected);
    this.#subscribeAll();
  }

  /**
   * Executes one admitted command. Return a {@link ParleyOutcome} value — expected failures are
   * values, not exceptions. Emit progress through `execution.reportProgress`, and observe
   * `execution.signal` (triggered by a wire-level `cnl`, H7). A thrown handler or an
   * unserializable/`$`-reserved custom error aborts the command (slot freed, timers cancelled,
   * id tombstoned) — never a leak; the commander then ends via its own timers.
   */
  protected abstract handleCommand(
    request: TRequest,
    execution: ParleyExecution<TProgress>,
  ): ParleyOutcome<TResult, TError> | Promise<ParleyOutcome<TResult, TError>>;

  #subscribeAll(): void {
    this.#adapter.subscribe(this.#topics.request, (_t, payload) => this.#onRequestTopic(payload));
  }

  #onRequestTopic(payload: Uint8Array): void {
    const env = parse(payload);
    if (env === null || env.v !== PROTOCOL_VERSION || !isValidId(env.id)) {
      return;
    }
    if (this.#disposed) {
      return;
    }

    switch (env.t) {
      case MessageType.Request:
        this.#onRequest(env.id, env.p);
        break;
      case MessageType.ResultAck:
        this.#onResultAck(env.id);
        break;
      case MessageType.Cancel:
        this.#onCancel(env.id);
        break;
      default:
        // Unknown type, or a handler→commander type on the request topic — ignore (§4).
        break;
    }
  }

  #onRequest(id: string, rawPayload: unknown): void {
    // H1: dedup FIRST — the capacity check applies only to unknown ids.
    const existing = this.#commands.get(id);
    if (existing) {
      if (existing.state === 'queued' || existing.state === 'executing') {
        this.#publishHeartbeat(existing); // immediate hb resynchronizes the commander cheaply
      } else {
        this.#republishCachedResult(existing);
      }
      return;
    }

    // A duplicate of a tombstoned id is ignored (H1).
    if (this.#tombstones.has(id)) {
      return;
    }

    // H2/H8: unknown id — admit if a command slot is free, otherwise stateless $busy.
    if (this.#commands.size >= this.#config.maxActiveCommands) {
      this.#publishStatelessError(id, ParleyBuiltinError.Busy);
      return;
    }

    let request: TRequest;
    try {
      request = this.#decodeRequest(rawPayload);
    } catch {
      // §4 robustness: a req whose payload cannot be decoded is silently ignored — no admission.
      return;
    }

    const abort = new ParleySignalSource();
    const cmd: HandlerCommand<TRequest, TProgress> = {
      id,
      request,
      abort,
      execution: new ParleyExecutionContext<TProgress>(id, abort.signal, (progress) =>
        this.#emitProgress(id, progress),
      ),
      state: 'queued',
    };
    // Mutate all state BEFORE publishing (re-entrancy safety), then the first hb is the accept-ack (H2).
    this.#queue.push(cmd);
    this.#commands.set(id, cmd);
    this.#publishHeartbeat(cmd);
    this.#tryDispatch();
  }

  #onResultAck(id: string): void {
    // H6: a rak for an id not in RESULT-CACHED is ignored.
    const cmd = this.#commands.get(id);
    if (cmd && cmd.state === 'result-cached') {
      this.#moveToTombstone(cmd);
    }
  }

  #onCancel(id: string): void {
    const cmd = this.#commands.get(id);
    if (cmd) {
      switch (cmd.state) {
        case 'executing':
          // H7: trigger the cancellation signal; the eventual outcome is what the handler produces.
          cmd.abort.abort();
          break;
        case 'queued':
          // H7: dequeue and produce a cached, retried $cancelled result.
          this.#removeFromQueue(cmd);
          this.#completeWithResult(
            cmd,
            this.#builtinErrorBytes(id, ParleyBuiltinError.Cancelled),
            false,
          );
          break;
        case 'result-cached':
          // H7: cancel lost the race — re-publish the cached result.
          this.#republishCachedResult(cmd);
          break;
      }
      return;
    }

    // H7: unknown or tombstoned id — stateless $cancelled (no state, no rak expected).
    this.#publishStatelessError(id, ParleyBuiltinError.Cancelled);
  }

  #tryDispatch(): void {
    if (this.#dispatching) {
      return;
    }
    this.#dispatching = true;
    try {
      // H4: dequeue FIFO into at most maxConcurrentExecutions slots.
      while (this.#executingCount < this.#config.maxConcurrentExecutions && this.#queue.length > 0) {
        const cmd = this.#queue.shift()!;
        cmd.state = 'executing';
        this.#executingCount++;
        void this.#runCommand(cmd).catch(() => {
          // runCommand contains all faults internally; this guards against any unforeseen rejection.
        });
      }
    } finally {
      this.#dispatching = false;
    }
  }

  async #runCommand(cmd: HandlerCommand<TRequest, TProgress>): Promise<void> {
    let outcome: ParleyOutcome<TResult, TError>;
    try {
      // Dispatch glue adds only microtask hops from here to the res publish (no macrotask/clock).
      outcome = await this.handleCommand(cmd.request, cmd.execution);
    } catch {
      // A thrown handler aborts the command so the slot is freed and state stays bounded. No result
      // is produced; the commander ends via its own timers.
      if (!this.#disposed) {
        this.#abortExecuting(cmd);
      }
      return;
    }

    if (this.#disposed) {
      return;
    }

    // Build the outcome bytes BEFORE mutating any state. A `$`-reserved/non-string custom error (or
    // an unserializable result) throws; the settled contract treats that exactly like a thrown
    // handler — abort so the slot is freed, timers cancelled, and the id tombstoned.
    let resultBytes: Uint8Array;
    try {
      resultBytes = this.#buildOutcomeBytes(cmd.id, outcome);
    } catch {
      this.#abortExecuting(cmd);
      return;
    }

    this.#completeWithResult(cmd, resultBytes, true);
  }

  #completeWithResult(
    cmd: HandlerCommand<TRequest, TProgress>,
    resultBytes: Uint8Array,
    wasExecuting: boolean,
  ): void {
    // H3: heartbeats run until the command's first res is published.
    cmd.heartbeatTimer?.cancel();
    cmd.heartbeatTimer = undefined;

    cmd.cachedResultBytes = resultBytes;
    cmd.state = 'result-cached';
    this.#publish(this.#topics.result, resultBytes);

    // H5: retry the result until rak or resultRetryTimeout (measured from this first publication).
    cmd.resultRetryTimer = this.#runtime.schedule(this.#config.resultRetryInterval, () =>
      this.#onResultRetry(cmd),
    );
    cmd.resultRetryTimeoutTimer = this.#runtime.schedule(this.#config.resultRetryTimeout, () =>
      this.#moveToTombstone(cmd),
    );

    if (wasExecuting) {
      this.#executingCount--;
      this.#tryDispatch();
    }
  }

  #abortExecuting(cmd: HandlerCommand<TRequest, TProgress>): void {
    cmd.heartbeatTimer?.cancel();
    cmd.heartbeatTimer = undefined;
    this.#executingCount--;
    this.#commands.delete(cmd.id);
    this.#tombstones.add(cmd.id, this.#config.tombstoneTtl);
    this.#tryDispatch();
  }

  #onResultRetry(cmd: HandlerCommand<TRequest, TProgress>): void {
    // Reschedule before publishing so a throwing adapter never kills the retry cadence (self-heals).
    cmd.resultRetryTimer?.reschedule(this.#config.resultRetryInterval);
    if (cmd.cachedResultBytes) {
      this.#publish(this.#topics.result, cmd.cachedResultBytes);
    }
  }

  #moveToTombstone(cmd: HandlerCommand<TRequest, TProgress>): void {
    cmd.resultRetryTimer?.cancel();
    cmd.resultRetryTimeoutTimer?.cancel();
    cmd.heartbeatTimer?.cancel();
    cmd.resultRetryTimer = undefined;
    cmd.resultRetryTimeoutTimer = undefined;
    cmd.heartbeatTimer = undefined;

    // H8: the admission slot is freed at the tombstone transition.
    this.#commands.delete(cmd.id);
    this.#tombstones.add(cmd.id, this.#config.tombstoneTtl);
  }

  #publishHeartbeat(cmd: HandlerCommand<TRequest, TProgress>): void {
    // The hb envelope is constant per id — serialize it once and reuse the cached bytes.
    cmd.cachedHeartbeatBytes ??= serialize({
      v: PROTOCOL_VERSION,
      t: MessageType.Heartbeat,
      id: cmd.id,
    });

    // H3: one cadence per id, restarted by every hb publication. (Re)schedule BEFORE publishing so a
    // throwing adapter never kills the cadence.
    if (!cmd.heartbeatTimer) {
      cmd.heartbeatTimer = this.#runtime.schedule(this.#config.heartbeatInterval, () =>
        this.#publishHeartbeat(cmd),
      );
    } else {
      cmd.heartbeatTimer.reschedule(this.#config.heartbeatInterval);
    }

    this.#publish(this.#topics.heartbeat, cmd.cachedHeartbeatBytes);
  }

  #republishCachedResult(cmd: HandlerCommand<TRequest, TProgress>): void {
    if (cmd.cachedResultBytes) {
      this.#publish(this.#topics.result, cmd.cachedResultBytes);
    }
  }

  #publishStatelessError(id: string, code: ParleyBuiltinErrorCode): void {
    this.#publish(this.#topics.result, this.#builtinErrorBytes(id, code));
  }

  #emitProgress(id: string, progress: TProgress): void {
    if (this.#disposed) {
      return;
    }
    const bytes = serialize({
      v: PROTOCOL_VERSION,
      t: MessageType.Progress,
      id,
      p: toPayload(this.#encodeProgress(progress)),
    });
    this.#publish(this.#topics.progress, bytes);
  }

  #buildOutcomeBytes(id: string, outcome: ParleyOutcome<TResult, TError>): Uint8Array {
    switch (outcome.kind) {
      case 'success':
        return serialize({
          v: PROTOCOL_VERSION,
          t: MessageType.Result,
          id,
          ok: true,
          p: toPayload(this.#encodeResult(outcome.value)),
        });
      case 'error': {
        const code = this.#validateCustomCode(outcome.error);
        return serialize({
          v: PROTOCOL_VERSION,
          t: MessageType.Result,
          id,
          ok: false,
          e: code,
          m: outcome.message,
        });
      }
      case 'cancelled':
        return this.#builtinErrorBytes(id, ParleyBuiltinError.Cancelled);
      default: {
        const never: never = outcome;
        throw new Error(`Unknown outcome kind: ${JSON.stringify(never)}`);
      }
    }
  }

  #validateCustomCode(error: TError): string {
    // $-prefix fail-fast (§5): custom codes must be plain strings not beginning with '$'.
    if (typeof error !== 'string') {
      throw new Error('A custom Parley error must serialize to a plain string code.');
    }
    if (error.startsWith('$')) {
      throw new Error(`Custom error codes must not begin with '$' (reserved for the protocol): '${error}'.`);
    }
    return error;
  }

  #builtinErrorBytes(id: string, code: ParleyBuiltinErrorCode): Uint8Array {
    return serialize({ v: PROTOCOL_VERSION, t: MessageType.Result, id, ok: false, e: code });
  }

  #decodeRequest(rawPayload: unknown): TRequest {
    if (this.#codec?.decodeRequest) {
      return this.#codec.decodeRequest(rawPayload);
    }
    return rawPayload as TRequest;
  }

  #encodeResult(value: TResult): unknown {
    if (this.#codec?.encodeResult) {
      return this.#codec.encodeResult(value);
    }
    return value;
  }

  #encodeProgress(value: TProgress): unknown {
    if (this.#codec?.encodeProgress) {
      return this.#codec.encodeProgress(value);
    }
    return value;
  }

  #removeFromQueue(cmd: HandlerCommand<TRequest, TProgress>): void {
    const index = this.#queue.indexOf(cmd);
    if (index >= 0) {
      this.#queue.splice(index, 1);
    }
  }

  #publish(topic: string, bytes: Uint8Array): void {
    try {
      this.#adapter.publish(topic, bytes);
    } catch {
      // Containment: adapter faults must never escape onto library paths (self-healing retries).
    }
  }

  /**
   * Disposes the handler (design.md): detaches the connected callback, unsubscribes the request
   * topic, cancels every per-command timer, signals every command's cancellation, and clears all
   * state. Idempotent. In-flight `handleCommand` calls that later resolve are dropped (no publish).
   */
  dispose(): void {
    if (this.#disposed) {
      return;
    }
    this.#disposed = true;

    this.#adapter.offConnected(this.#onConnected);
    this.#adapter.unsubscribe(this.#topics.request);

    const sources: ParleySignalSource[] = [];
    for (const cmd of this.#commands.values()) {
      cmd.heartbeatTimer?.cancel();
      cmd.resultRetryTimer?.cancel();
      cmd.resultRetryTimeoutTimer?.cancel();
      sources.push(cmd.abort);
    }

    this.#commands.clear();
    this.#queue.length = 0;
    this.#executingCount = 0;
    this.#tombstones.clear();

    // Signal any executing handler to stop (a consumer abort listener is contained by ParleySignalSource).
    for (const source of sources) {
      source.abort();
    }
  }

  // --- internal inspection hooks for the bounded-state tests (see test/support) ---
  /** @internal */
  get _commandCount(): number {
    return this.#commands.size;
  }

  /** @internal */
  get _queueCount(): number {
    return this.#queue.length;
  }

  /** @internal */
  get _executingCount(): number {
    return this.#executingCount;
  }

  /** @internal */
  get _tombstoneCount(): number {
    return this.#tombstones.count;
  }

  /** @internal */
  get _liveTimerCount(): number {
    return this.#runtime.liveTimerCount;
  }
}
