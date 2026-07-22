import type { ParleyConnectionAdapter } from './adapter';
import { SystemClock, type ParleyClock } from './clock';
import {
  resolveCallOptions,
  resolveConfig,
  type ParleyCallOptions,
  type ParleyConfig,
  type ParleyTopics,
  type ResolvedCallOptions,
  type ResolvedConfig,
} from './config';
import {
  ParleyBuiltinError,
  toBuiltin,
  type ParleyBuiltinErrorCode,
  type ParleyError,
  type ParleyResult,
} from './errors';
import {
  isValidId,
  MessageType,
  parse,
  PROTOCOL_VERSION,
  serialize,
  toPayload,
  type InboundEnvelope,
  type MessageTypeValue,
} from './envelope';
import { DefaultIdGenerator, type ParleyIdGenerator } from './idGenerator';
import { ParleyRuntime, type ParleyTimer } from './runtime';
import { TombstoneTable } from './tombstones';
import type { ParleyAbortSignal } from './execution';

/** Optional per-commander codec hooks. All default to a raw pass-through. */
export interface ParleyCommanderCodec<TRequest, TResult, TProgress> {
  /**
   * Encodes/validates a request payload before it is serialized onto the wire. Throwing surfaces
   * synchronously out of {@link ParleyCommander.execute} as a caller-visible programming error
   * (bad codec/payload) — no command state is created. Default: pass-through.
   */
  encodeRequest?(value: TRequest): unknown;
  /**
   * Decodes/validates a success `res` payload. Throwing yields `$decodeError` (a `rak` is still
   * published, per C4). Default: the raw JSON value is the result.
   */
  decodeResult?(raw: unknown): TResult;
  /** Decodes a `prg` payload. Throwing drops the progress update silently (C8). Default: pass-through. */
  decodeProgress?(raw: unknown): TProgress;
}

/** Construction-time seams and codec for a {@link ParleyCommander}. */
export interface ParleyCommanderOptions<TRequest, TResult, TProgress> {
  codec?: ParleyCommanderCodec<TRequest, TResult, TProgress>;
  idGenerator?: ParleyIdGenerator;
  clock?: ParleyClock;
}

/** Per-call options for {@link ParleyCommander.execute}: timing overrides plus progress + signal. */
export interface ParleyExecuteOptions<TProgress> extends ParleyCallOptions {
  /** Advisory progress sink for `prg` updates (C8). */
  onProgress?: (progress: TProgress) => void;
  /** Abort signal; aborting triggers a wire-level `cnl` (C7). Any duck-typed signal is accepted. */
  signal?: ParleyAbortSignal;
}

type CommandState = 'sending' | 'live';

interface Deferred<T> {
  readonly promise: Promise<T>;
  readonly resolve: (value: T) => void;
}

function createDeferred<T>(): Deferred<T> {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>((r) => {
    resolve = r;
  });
  return { promise, resolve };
}

interface Command<TProgress, TResult, TError> {
  readonly id: string;
  readonly deferred: Deferred<ParleyResult<TResult, TError>>;
  readonly onProgress: ((progress: TProgress) => void) | undefined;
  readonly options: ResolvedCallOptions;
  readonly requestBytes: Uint8Array;
  state: CommandState;
  cancelling: boolean;
  cancelBytes?: Uint8Array;
  requestRetryTimer?: ParleyTimer;
  signOfLifeTimer?: ParleyTimer;
  executionCapTimer?: ParleyTimer;
  cancelRetryTimer?: ParleyTimer;
  signal?: ParleyAbortSignal;
  signalListener?: () => void;
}

/**
 * The caller side of one command type (protocol.md §6, rules C1–C8). Multiple commanders MAY share a
 * topic set; each filters inbound traffic by command id. Expected outcomes — including all six
 * builtin codes — resolve as {@link ParleyResult} values, never rejections.
 */
export class ParleyCommander<TRequest, TProgress, TResult, TError = string> {
  readonly #adapter: ParleyConnectionAdapter;
  readonly #topics: ParleyTopics;
  readonly #config: ResolvedConfig;
  readonly #runtime: ParleyRuntime;
  readonly #tombstones: TombstoneTable;
  readonly #idGenerator: ParleyIdGenerator;
  readonly #codec: ParleyCommanderCodec<TRequest, TResult, TProgress> | undefined;
  readonly #inflight = new Map<string, Command<TProgress, TResult, TError>>();
  readonly #onConnected = (): void => {
    if (this.#disposed) {
      return;
    }
    this.#subscribeAll();
  };
  #disposed = false;

  constructor(
    adapter: ParleyConnectionAdapter,
    topics: ParleyTopics,
    config: ParleyConfig = {},
    options: ParleyCommanderOptions<TRequest, TResult, TProgress> = {},
  ) {
    this.#adapter = adapter;
    this.#topics = topics;
    this.#config = resolveConfig(config);
    this.#runtime = new ParleyRuntime(options.clock ?? new SystemClock());
    this.#tombstones = new TombstoneTable(this.#runtime, this.#config.maxTombstones);
    this.#idGenerator = options.idGenerator ?? DefaultIdGenerator.instance;
    this.#codec = options.codec;

    this.#adapter.onConnected(this.#onConnected);
    this.#subscribeAll();
  }

  #subscribeAll(): void {
    this.#adapter.subscribe(this.#topics.heartbeat, (_t, payload) =>
      this.#onInbound(payload, MessageType.Heartbeat),
    );
    this.#adapter.subscribe(this.#topics.progress, (_t, payload) =>
      this.#onInbound(payload, MessageType.Progress),
    );
    this.#adapter.subscribe(this.#topics.result, (_t, payload) =>
      this.#onInbound(payload, MessageType.Result),
    );
  }

  /**
   * Executes one command and awaits its single terminal outcome (protocol.md C1–C5). Aborting
   * `options.signal` triggers a wire-level `cnl` (C7); the returned result is still whatever the
   * command actually reaches (a genuine result may win the race). Never rejects for an expected
   * outcome. Throws synchronously on misuse (disposed instance, invalid/colliding id) or when a
   * configured `codec.encodeRequest` throws (a caller-visible programming error, fail-fast like the
   * config/id validation — no command state is created).
   */
  execute(
    request: TRequest,
    options?: ParleyExecuteOptions<TProgress>,
  ): Promise<ParleyResult<TResult, TError>> {
    if (this.#disposed) {
      throw new Error('ParleyCommander is disposed.');
    }

    const id = this.#idGenerator.newId();
    if (!isValidId(id)) {
      throw new Error(`Command-id generator produced an invalid id: '${id}'.`);
    }
    if (this.#inflight.has(id) || this.#tombstones.has(id)) {
      throw new Error(`Command id '${id}' collides with existing state.`);
    }

    // AB2: encode the request up front. A throwing encodeRequest is a caller-visible programming
    // error — let it propagate synchronously before ANY per-command state (inflight, timers, signal
    // listener) exists, so a failed encode leaves nothing to unwind. Consistent with fail-fast config.
    const requestBytes = serialize({
      v: PROTOCOL_VERSION,
      t: MessageType.Request,
      id,
      p: toPayload(this.#encodeRequest(request)),
    });

    const deferred = createDeferred<ParleyResult<TResult, TError>>();
    const cmd: Command<TProgress, TResult, TError> = {
      id,
      deferred,
      onProgress: options?.onProgress,
      options: resolveCallOptions(this.#config, options),
      requestBytes,
      state: 'sending',
      cancelling: false,
    };
    this.#inflight.set(id, cmd);

    // design.md — "Synchronous delivery is legal; publish comes last": schedule the request-retry and
    // sign-of-life timers BEFORE the initial publish, so a synchronous-delivery adapter that re-enters
    // (e.g. an `hb`/`res` delivered from inside publish) finds real timers to cancel/reschedule.
    cmd.requestRetryTimer = this.#runtime.schedule(cmd.options.requestRetryInterval, () =>
      this.#onRequestRetry(cmd),
    );
    cmd.signOfLifeTimer = this.#runtime.schedule(cmd.options.noAckTimeout, () =>
      this.#onSignOfLifeExpiry(cmd),
    );

    this.#publish(this.#topics.request, cmd.requestBytes);

    // Signal wiring stays AFTER the initial publish so the wire order is req-then-cnl for an
    // already-aborted signal. Guard on still-in-flight: a synchronous reply during the publish above
    // may already have completed and torn the command down (no listener to attach then).
    const signal = options?.signal;
    if (signal && this.#inflight.has(id)) {
      if (signal.aborted) {
        this.#requestCancel(id);
      } else {
        const listener = (): void => this.#requestCancel(id);
        signal.addEventListener('abort', listener);
        cmd.signal = signal;
        cmd.signalListener = listener;
      }
    }

    return deferred.promise;
  }

  #requestCancel(id: string): void {
    const cmd = this.#inflight.get(id);
    if (this.#disposed || !cmd || cmd.cancelling) {
      return;
    }
    cmd.cancelling = true;

    // C7: a command the caller no longer wants must not keep being offered for admission.
    if (cmd.state === 'sending') {
      cmd.requestRetryTimer?.cancel();
      cmd.requestRetryTimer = undefined;
    }

    cmd.cancelBytes = serialize({ v: PROTOCOL_VERSION, t: MessageType.Cancel, id });
    // design.md — schedule the cancel-retry timer BEFORE publishing the `cnl`. A synchronous-delivery
    // adapter can deliver the `$cancelled` `res` from inside this publish and tear the command down;
    // the re-entrant teardown must find the timer to cancel it, otherwise it orphans a self-rearming
    // timer that re-publishes `cnl` forever (a permanent cnl-storm leak).
    cmd.cancelRetryTimer = this.#runtime.schedule(cmd.options.cancelRetryInterval, () =>
      this.#onCancelRetry(cmd),
    );
    this.#publish(this.#topics.request, cmd.cancelBytes);
  }

  #onRequestRetry(cmd: Command<TProgress, TResult, TError>): void {
    // Reschedule before publishing so a throwing adapter never kills the cadence (self-heals).
    cmd.requestRetryTimer?.reschedule(cmd.options.requestRetryInterval);
    this.#publish(this.#topics.request, cmd.requestBytes);
  }

  #onCancelRetry(cmd: Command<TProgress, TResult, TError>): void {
    cmd.cancelRetryTimer?.reschedule(cmd.options.cancelRetryInterval);
    if (cmd.cancelBytes) {
      this.#publish(this.#topics.request, cmd.cancelBytes);
    }
  }

  #onSignOfLifeExpiry(cmd: Command<TProgress, TResult, TError>): void {
    // SENDING → $noAck (never any sign of life); LIVE → $executionStale (went silent).
    this.#finishBuiltin(
      cmd,
      cmd.state === 'sending' ? ParleyBuiltinError.NoAck : ParleyBuiltinError.ExecutionStale,
    );
  }

  #onInbound(payload: Uint8Array, expectedType: MessageTypeValue): void {
    const env = parse(payload);
    if (env === null || env.v !== PROTOCOL_VERSION || env.t !== expectedType || !isValidId(env.id)) {
      return;
    }
    if (this.#disposed) {
      return;
    }

    switch (expectedType) {
      case MessageType.Heartbeat:
        this.#onHeartbeat(env.id);
        break;
      case MessageType.Progress:
        this.#onProgress(env.id, env.p);
        break;
      case MessageType.Result:
        this.#onResult(env);
        break;
      default:
        break;
    }
  }

  #onHeartbeat(id: string): void {
    const cmd = this.#inflight.get(id);
    if (cmd) {
      this.#onSignOfLife(cmd);
    }
  }

  #onProgress(id: string, rawPayload: unknown): void {
    const cmd = this.#inflight.get(id);
    if (!cmd) {
      return;
    }

    this.#onSignOfLife(cmd);

    const sink = cmd.onProgress;
    if (sink) {
      let value: TProgress;
      try {
        value = this.#decodeProgress(rawPayload);
      } catch {
        // C8: progress delivery is advisory — a prg that fails to decode is dropped (still a sign of life).
        return;
      }
      try {
        sink(value);
      } catch {
        // Containment: a consumer progress callback must never escape onto library paths.
      }
    }
  }

  #onResult(env: InboundEnvelope): void {
    const id = env.id;

    // ok is required on res; e is required when ok:false (§4).
    if (typeof env.ok !== 'boolean') {
      return;
    }
    const ok = env.ok;
    if (ok === false && env.e === undefined) {
      return;
    }

    const isBusy = ok === false && env.e === ParleyBuiltinError.Busy;
    const cmd = this.#inflight.get(id);

    if (cmd) {
      // C4 exception: a $busy res in LIVE can only be a stale pre-admission reply — ignore it (and it
      // is NOT a sign of life, so the sliding window is not reset).
      if (cmd.state === 'live' && isBusy) {
        return;
      }

      if (ok) {
        // C4: decode the payload BEFORE acking. A success payload that fails to decode is a peer
        // contract mismatch — still publish rak (redelivery cannot fix it) and terminate $decodeError.
        let value: TResult;
        let decoded = true;
        try {
          value = this.#decodeResult(env.p);
        } catch {
          decoded = false;
          value = undefined as TResult;
        }

        this.#publishResultAck(id);
        if (decoded) {
          this.#finish(cmd, { ok: true, value });
        } else {
          this.#finishBuiltin(cmd, ParleyBuiltinError.DecodeError);
        }
      } else {
        // C4: rak every terminal error res except $busy (the handler holds no state for a refused id).
        if (!isBusy) {
          this.#publishResultAck(id);
        }
        this.#finish(cmd, { ok: false, error: this.#buildError(env.e!, env.m) });
      }
      return;
    }

    // C5: a res for a tombstoned id re-emits rak (unless $busy) and discards the content.
    if (this.#tombstones.has(id)) {
      if (!isBusy) {
        this.#publishResultAck(id);
      }
      return;
    }

    // C6: neither in-flight nor tombstoned — belongs to another commander instance. Ignore.
  }

  #onSignOfLife(cmd: Command<TProgress, TResult, TError>): void {
    if (cmd.state === 'sending') {
      // C2: first sign of life — enter LIVE, stop req retries, start the execution cap.
      cmd.state = 'live';
      cmd.requestRetryTimer?.cancel();
      cmd.requestRetryTimer = undefined;

      const cap = cmd.options.executionTimeout;
      if (cap !== null) {
        cmd.executionCapTimer = this.#runtime.schedule(cap, () =>
          this.#finishBuiltin(cmd, ParleyBuiltinError.ExecutionTimeout),
        );
      }
    }

    // The sliding sign-of-life timer restarts on every sign of life (C2/C3).
    cmd.signOfLifeTimer?.reschedule(cmd.options.staleTimeout);
  }

  #encodeRequest(value: TRequest): unknown {
    if (this.#codec?.encodeRequest) {
      return this.#codec.encodeRequest(value);
    }
    return value;
  }

  #decodeResult(rawPayload: unknown): TResult {
    if (this.#codec?.decodeResult) {
      return this.#codec.decodeResult(rawPayload);
    }
    return rawPayload as TResult;
  }

  #decodeProgress(rawPayload: unknown): TProgress {
    if (this.#codec?.decodeProgress) {
      return this.#codec.decodeProgress(rawPayload);
    }
    return rawPayload as TProgress;
  }

  #buildError(code: string, message: string | undefined): ParleyError<TError> {
    const builtin = toBuiltin(code);
    if (builtin !== undefined) {
      return message === undefined ? { code, builtin } : { code, builtin, message };
    }
    // Custom errors derive from the e string code only (structured error p is not surfaced in v1).
    const custom = code as unknown as TError;
    return message === undefined ? { code, custom } : { code, custom, message };
  }

  #finishBuiltin(
    cmd: Command<TProgress, TResult, TError>,
    builtin: ParleyBuiltinErrorCode,
  ): void {
    this.#finish(cmd, { ok: false, error: { code: builtin, builtin } });
  }

  #finish(
    cmd: Command<TProgress, TResult, TError>,
    result: ParleyResult<TResult, TError>,
  ): void {
    if (!this.#inflight.has(cmd.id)) {
      return;
    }
    this.#teardown(cmd);
    cmd.deferred.resolve(result);
  }

  #teardown(cmd: Command<TProgress, TResult, TError>): void {
    cmd.requestRetryTimer?.cancel();
    cmd.signOfLifeTimer?.cancel();
    cmd.executionCapTimer?.cancel();
    cmd.cancelRetryTimer?.cancel();
    cmd.requestRetryTimer = undefined;
    cmd.signOfLifeTimer = undefined;
    cmd.executionCapTimer = undefined;
    cmd.cancelRetryTimer = undefined;
    if (cmd.signal && cmd.signalListener) {
      cmd.signal.removeEventListener?.('abort', cmd.signalListener);
    }
    cmd.signal = undefined;
    cmd.signalListener = undefined;

    this.#inflight.delete(cmd.id);
    // C5: every terminal outcome tombstones the id for tombstoneTtl.
    this.#tombstones.add(cmd.id, cmd.options.tombstoneTtl);
  }

  #publishResultAck(id: string): void {
    this.#publish(
      this.#topics.request,
      serialize({ v: PROTOCOL_VERSION, t: MessageType.ResultAck, id }),
    );
  }

  #publish(topic: string, bytes: Uint8Array): void {
    try {
      this.#adapter.publish(topic, bytes);
    } catch {
      // Containment: adapter faults (publish on a disconnected/disposed client) must never escape
      // onto library paths. Retries reschedule before publishing, so delivery self-heals.
    }
  }

  /**
   * Disposes the commander (design.md): detaches the connected callback, unsubscribes the library's
   * topics, cancels every per-command timer, completes every in-flight call locally with
   * `$cancelled`, and clears all state. Idempotent. After disposal, {@link execute} throws and
   * inbound transport callbacks are ignored.
   */
  dispose(): void {
    if (this.#disposed) {
      return;
    }
    this.#disposed = true;

    this.#adapter.offConnected(this.#onConnected);
    this.#adapter.unsubscribe(this.#topics.heartbeat);
    this.#adapter.unsubscribe(this.#topics.progress);
    this.#adapter.unsubscribe(this.#topics.result);

    const cancelled: ParleyResult<TResult, TError> = {
      ok: false,
      error: { code: ParleyBuiltinError.Cancelled, builtin: ParleyBuiltinError.Cancelled },
    };

    for (const cmd of this.#inflight.values()) {
      cmd.requestRetryTimer?.cancel();
      cmd.signOfLifeTimer?.cancel();
      cmd.executionCapTimer?.cancel();
      cmd.cancelRetryTimer?.cancel();
      if (cmd.signal && cmd.signalListener) {
        cmd.signal.removeEventListener?.('abort', cmd.signalListener);
      }
      cmd.deferred.resolve(cancelled);
    }

    this.#inflight.clear();
    this.#tombstones.clear();
  }

  // --- internal inspection hooks for the bounded-state tests (see test/support) ---
  /** @internal */
  get _inflightCount(): number {
    return this.#inflight.size;
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
