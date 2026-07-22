import type { ParleyClock, ParleyTimerHandle } from './clock';

/**
 * The timer scheduler for one Parley instance over a {@link ParleyClock}. Every scheduled timer is
 * tracked so state stays bounded (`liveTimerCount` is asserted by the bounded-state tests). JS is
 * single-threaded, so no gate is needed; a cancelled clock timer never fires, so no generation guard
 * is required either — {@link ParleyTimer} keeps a `disposed` flag purely as belt-and-suspenders.
 */
export class ParleyRuntime {
  readonly #clock: ParleyClock;
  #liveTimers = 0;

  constructor(clock: ParleyClock) {
    this.#clock = clock;
  }

  /** Number of live (undisposed) timers. */
  get liveTimerCount(): number {
    return this.#liveTimers;
  }

  /** Schedules a one-shot callback after `delayMs`. */
  schedule(delayMs: number, callback: () => void): ParleyTimer {
    return new ParleyTimer(this, this.#clock, delayMs, callback);
  }

  /** @internal */
  _increment(): void {
    this.#liveTimers++;
  }

  /** @internal */
  _decrement(): void {
    this.#liveTimers--;
  }
}

/** A single logical timer: one-shot by default, rescheduled or cancelled explicitly. */
export class ParleyTimer {
  readonly #runtime: ParleyRuntime;
  readonly #clock: ParleyClock;
  readonly #callback: () => void;
  #handle: ParleyTimerHandle;
  #disposed = false;

  constructor(runtime: ParleyRuntime, clock: ParleyClock, delayMs: number, callback: () => void) {
    this.#runtime = runtime;
    this.#clock = clock;
    this.#callback = callback;
    runtime._increment();
    this.#handle = clock.schedule(() => this.#fire(), delayMs);
  }

  #fire(): void {
    if (this.#disposed) {
      return;
    }
    this.#callback();
  }

  /** Re-arms this timer to fire once more after `delayMs`. Does not change the live-timer count. */
  reschedule(delayMs: number): void {
    if (this.#disposed) {
      return;
    }
    this.#clock.cancel(this.#handle);
    this.#handle = this.#clock.schedule(() => this.#fire(), delayMs);
  }

  /** Stops and releases the timer. Idempotent. */
  cancel(): void {
    if (this.#disposed) {
      return;
    }
    this.#disposed = true;
    this.#clock.cancel(this.#handle);
    this.#runtime._decrement();
  }
}
