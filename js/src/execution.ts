/**
 * A minimal duck-typed abort signal (edge-engine floor: no hard `AbortController` dependency). A
 * standard `AbortSignal` satisfies this; the commander accepts any object with these members, and
 * the handler's {@link ParleyExecution.signal} is a Parley-provided instance.
 */
export interface ParleyAbortSignal {
  /** Whether cancellation has been requested. */
  readonly aborted: boolean;
  /** Registers an abort listener. */
  addEventListener(type: 'abort', listener: () => void): void;
  /** Deregisters an abort listener (optional; used by the commander to release on teardown). */
  removeEventListener?(type: 'abort', listener: () => void): void;
}

/**
 * The execution context handed to a handler: the command id, a cancellation signal triggered by a
 * wire-level `cnl` (protocol.md H7), and a progress emitter (each call publishes one `prg`, C8/H4).
 */
export interface ParleyExecution<TProgress> {
  /** The command id being executed (protocol.md §4.2). */
  readonly commandId: string;
  /** Signalled when a `cnl` for this command arrives. */
  readonly signal: ParleyAbortSignal;
  /** Emits one advisory progress update. Delivery is best-effort and never acknowledged. */
  reportProgress(progress: TProgress): void;
}

/**
 * Internal minimal signal source used by the handler for each admitted command. Not exported: the
 * handler owns creation and firing; the consumer only sees the read-only {@link ParleyAbortSignal}.
 */
export class ParleySignalSource {
  #aborted = false;
  #listeners: Array<() => void> = [];
  readonly signal: ParleyAbortSignal;

  constructor() {
    // Arrow closures capture this instance directly (no `this` aliasing); the read-only view exposes
    // `aborted` via a getter so it always reflects the current state.
    const signal: ParleyAbortSignal = {
      aborted: false,
      addEventListener: (_type: 'abort', listener: () => void): void => {
        this.#listeners.push(listener);
      },
      removeEventListener: (_type: 'abort', listener: () => void): void => {
        const index = this.#listeners.indexOf(listener);
        if (index >= 0) {
          this.#listeners.splice(index, 1);
        }
      },
    };
    Object.defineProperty(signal, 'aborted', {
      get: (): boolean => this.#aborted,
      enumerable: true,
    });
    this.signal = signal;
  }

  abort(): void {
    if (this.#aborted) {
      return;
    }
    this.#aborted = true;
    const listeners = this.#listeners;
    this.#listeners = [];
    for (const listener of listeners) {
      try {
        listener();
      } catch {
        // Containment: a consumer abort listener must never escape onto library paths.
      }
    }
  }
}

/** Concrete {@link ParleyExecution} handed to the handler; emits progress through a bound callback. */
export class ParleyExecutionContext<TProgress> implements ParleyExecution<TProgress> {
  readonly commandId: string;
  readonly signal: ParleyAbortSignal;
  readonly #emit: (progress: TProgress) => void;

  constructor(commandId: string, signal: ParleyAbortSignal, emit: (progress: TProgress) => void) {
    this.commandId = commandId;
    this.signal = signal;
    this.#emit = emit;
  }

  reportProgress(progress: TProgress): void {
    this.#emit(progress);
  }
}
