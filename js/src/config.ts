/**
 * The four consumer-supplied topic strings identifying one command type (protocol.md §3). Opaque,
 * distinct within the set, no MQTT wildcards.
 */
export class ParleyTopics {
  /** Commander → handler topic, carrying `req`, `rak`, and `cnl`. */
  readonly request: string;
  /** Handler → commander topic, carrying `hb`. */
  readonly heartbeat: string;
  /** Handler → commander topic, carrying `prg`. */
  readonly progress: string;
  /** Handler → commander topic, carrying `res`. */
  readonly result: string;

  constructor(request: string, heartbeat: string, progress: string, result: string) {
    this.request = validateTopic(request, 'request');
    this.heartbeat = validateTopic(heartbeat, 'heartbeat');
    this.progress = validateTopic(progress, 'progress');
    this.result = validateTopic(result, 'result');

    const set = new Set([this.request, this.heartbeat, this.progress, this.result]);
    if (set.size !== 4) {
      throw new Error('Parley topics must be distinct within the set.');
    }
  }
}

function validateTopic(topic: string, name: string): string {
  if (typeof topic !== 'string' || topic.length === 0) {
    throw new Error(`Parley topic '${name}' must be a non-empty string.`);
  }
  if (topic.includes('+') || topic.includes('#')) {
    throw new Error(`Parley topic '${name}' must not contain MQTT wildcards ('+' or '#').`);
  }
  return topic;
}

/**
 * Per-deployment timing configuration and handler capacity caps (protocol.md §8). All fields are
 * optional and fall back to the §8 defaults. Durations are in milliseconds.
 *
 * `executionTimeout` is tri-state: a number sets the cap, `null` disables it (no cap), and
 * `undefined`/absent inherits the default (60_000).
 */
export interface ParleyConfig {
  /** C1 — `req` re-publish interval while SENDING. Default 2000. */
  requestRetryInterval?: number;
  /** C3 — sign-of-life window before any sign of life (SENDING). Default 10000. */
  noAckTimeout?: number;
  /** C3 — sliding sign-of-life window after signs of life (LIVE). Default 15000. */
  staleTimeout?: number;
  /** C2/C3 — overall execution cap measured from the first sign of life. `null` disables. Default 60000. */
  executionTimeout?: number | null;
  /** C7 — `cnl` re-publish interval while cancelling. Default 2000. */
  cancelRetryInterval?: number;
  /** H3 — `hb` interval per admitted command. Default 3000. */
  heartbeatInterval?: number;
  /** H5 — `res` re-publish interval until acked. Default 2000. */
  resultRetryInterval?: number;
  /** H5 — result-retry give-up window, from the first `res` publication. Default 30000. */
  resultRetryTimeout?: number;
  /** C5/H5 — tombstone retention for duplicate suppression. Default 60000. */
  tombstoneTtl?: number;
  /** H4 — maximum commands executing concurrently. Default 1 (serial). */
  maxConcurrentExecutions?: number;
  /** H2/H8 — admission cap; a slot is held from admission through RESULT-CACHED. Default 32. */
  maxActiveCommands?: number;
  /** Hard cap on the tombstone table (oldest-first eviction). Default 4096. */
  maxTombstones?: number;
}

/** Per-call overrides of the commander-local timing parameters (protocol.md §8). */
export interface ParleyCallOptions {
  /** Overrides {@link ParleyConfig.requestRetryInterval}. */
  requestRetryInterval?: number;
  /** Overrides {@link ParleyConfig.noAckTimeout}. */
  noAckTimeout?: number;
  /** Overrides {@link ParleyConfig.staleTimeout}. */
  staleTimeout?: number;
  /** Overrides {@link ParleyConfig.executionTimeout}. A number sets the cap, `null` disables it for
   *  this call (overriding a configured default), `undefined`/absent inherits the config value. */
  executionTimeout?: number | null;
  /** Overrides {@link ParleyConfig.cancelRetryInterval}. */
  cancelRetryInterval?: number;
  /** Overrides {@link ParleyConfig.tombstoneTtl}. */
  tombstoneTtl?: number;
}

/** Fully-resolved config with every field present (`executionTimeout` may be `null` = disabled). */
export interface ResolvedConfig {
  readonly requestRetryInterval: number;
  readonly noAckTimeout: number;
  readonly staleTimeout: number;
  readonly executionTimeout: number | null;
  readonly cancelRetryInterval: number;
  readonly heartbeatInterval: number;
  readonly resultRetryInterval: number;
  readonly resultRetryTimeout: number;
  readonly tombstoneTtl: number;
  readonly maxConcurrentExecutions: number;
  readonly maxActiveCommands: number;
  readonly maxTombstones: number;
}

/** Per-call resolved commander timing (all present; `executionTimeout` may be `null`). */
export interface ResolvedCallOptions {
  readonly requestRetryInterval: number;
  readonly noAckTimeout: number;
  readonly staleTimeout: number;
  readonly executionTimeout: number | null;
  readonly cancelRetryInterval: number;
  readonly tombstoneTtl: number;
}

function requirePositive(value: number, name: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value) || value <= 0) {
    throw new Error(`Parley config '${name}' must be a strictly positive number of milliseconds.`);
  }
  return value;
}

function requireCount(value: number, name: string): number {
  if (typeof value !== 'number' || !Number.isInteger(value) || value < 1) {
    throw new Error(`Parley config '${name}' must be an integer >= 1.`);
  }
  return value;
}

/** Resolves and validates a {@link ParleyConfig}, applying the protocol.md §8 defaults. */
export function resolveConfig(config: ParleyConfig): ResolvedConfig {
  // executionTimeout is tri-state — branch explicitly (never ?? / ||, which would collapse a
  // null-disable into the default).
  let executionTimeout: number | null;
  if (config.executionTimeout === undefined) {
    executionTimeout = 60_000;
  } else if (config.executionTimeout === null) {
    executionTimeout = null;
  } else {
    executionTimeout = requirePositive(config.executionTimeout, 'executionTimeout');
  }

  return {
    requestRetryInterval: requirePositive(config.requestRetryInterval ?? 2_000, 'requestRetryInterval'),
    noAckTimeout: requirePositive(config.noAckTimeout ?? 10_000, 'noAckTimeout'),
    staleTimeout: requirePositive(config.staleTimeout ?? 15_000, 'staleTimeout'),
    executionTimeout,
    cancelRetryInterval: requirePositive(config.cancelRetryInterval ?? 2_000, 'cancelRetryInterval'),
    heartbeatInterval: requirePositive(config.heartbeatInterval ?? 3_000, 'heartbeatInterval'),
    resultRetryInterval: requirePositive(config.resultRetryInterval ?? 2_000, 'resultRetryInterval'),
    resultRetryTimeout: requirePositive(config.resultRetryTimeout ?? 30_000, 'resultRetryTimeout'),
    tombstoneTtl: requirePositive(config.tombstoneTtl ?? 60_000, 'tombstoneTtl'),
    maxConcurrentExecutions: requireCount(config.maxConcurrentExecutions ?? 1, 'maxConcurrentExecutions'),
    maxActiveCommands: requireCount(config.maxActiveCommands ?? 32, 'maxActiveCommands'),
    maxTombstones: requireCount(config.maxTombstones ?? 4_096, 'maxTombstones'),
  };
}

/**
 * Resolves per-call commander options against the config. `executionTimeout` uses the tri-state
 * rule (explicit `=== undefined` / `=== null` branching, never `??`/`||`): `null` disables the cap
 * for this call even when the config sets one; `undefined`/absent inherits the config value.
 */
export function resolveCallOptions(
  config: ResolvedConfig,
  options: ParleyCallOptions | undefined,
): ResolvedCallOptions {
  let executionTimeout: number | null;
  if (options === undefined || options.executionTimeout === undefined) {
    executionTimeout = config.executionTimeout;
  } else if (options.executionTimeout === null) {
    executionTimeout = null;
  } else {
    executionTimeout = options.executionTimeout;
  }

  return {
    requestRetryInterval: options?.requestRetryInterval ?? config.requestRetryInterval,
    noAckTimeout: options?.noAckTimeout ?? config.noAckTimeout,
    staleTimeout: options?.staleTimeout ?? config.staleTimeout,
    executionTimeout,
    cancelRetryInterval: options?.cancelRetryInterval ?? config.cancelRetryInterval,
    tombstoneTtl: options?.tombstoneTtl ?? config.tombstoneTtl,
  };
}
