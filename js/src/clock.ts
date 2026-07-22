/**
 * The clock seam (design.md). All timing in the core flows through this interface so the test
 * suites can drive a deterministic virtual clock and production defaults to the ambient timer.
 *
 * A handle is opaque: it is whatever {@link ParleyClock.schedule} returns and is only ever passed
 * back to {@link ParleyClock.cancel}.
 */
export type ParleyTimerHandle = unknown;

/** A minimal scheduling clock. Defaults to {@link SystemClock}; overridden by the test suites. */
export interface ParleyClock {
  /** Current time in milliseconds (monotonic-ish; the core does not depend on the epoch). */
  now(): number;
  /** Schedules a one-shot callback after `delayMs`, returning a handle. */
  schedule(callback: () => void, delayMs: number): ParleyTimerHandle;
  /** Cancels a scheduled callback. A callback that has already fired is unaffected. */
  cancel(handle: ParleyTimerHandle): void;
}

/**
 * The default production clock over the ambient `setTimeout`/`clearTimeout`/`Date.now`. These are
 * standard globals on every engine in the v1 floor (Node >= 18, Deno, Bun, workerd, QuickJS-ng);
 * the core references no Node built-ins.
 */
export class SystemClock implements ParleyClock {
  now(): number {
    return Date.now();
  }

  schedule(callback: () => void, delayMs: number): ParleyTimerHandle {
    return setTimeout(callback, delayMs);
  }

  cancel(handle: ParleyTimerHandle): void {
    clearTimeout(handle as ReturnType<typeof setTimeout>);
  }
}
