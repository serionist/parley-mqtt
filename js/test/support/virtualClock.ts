import type { ParleyClock, ParleyTimerHandle } from '../../src/index';

interface ScheduledTimer {
  readonly handle: number;
  readonly dueAt: number;
  readonly seq: number;
  readonly callback: () => void;
}

/**
 * A deterministic virtual clock satisfying the protocol.md §10 determinism rule: on {@link advanceTo},
 * every timer due at or before the target fires in due-order (insertion order on ties) before time
 * settles at the target. Advancing is the only thing that moves virtual time; draining microtasks
 * (see drain.ts) never touches it.
 */
export class VirtualClock implements ParleyClock {
  #now = 0;
  #seq = 0;
  readonly #timers = new Map<number, ScheduledTimer>();

  now(): number {
    return this.#now;
  }

  schedule(callback: () => void, delayMs: number): ParleyTimerHandle {
    const handle = this.#seq++;
    this.#timers.set(handle, { handle, dueAt: this.#now + delayMs, seq: handle, callback });
    return handle;
  }

  cancel(handle: ParleyTimerHandle): void {
    this.#timers.delete(handle as number);
  }

  /** Number of pending (uncancelled, unfired) timers — sanity check for the runner. */
  get pendingCount(): number {
    return this.#timers.size;
  }

  /**
   * Advances virtual time to `target`, firing every timer due at or before it in (dueAt, seq) order.
   * A callback that reschedules/schedules a timer still due at or before `target` fires within the
   * same advance. Time never moves backward.
   */
  advanceTo(target: number): void {
    if (target < this.#now) {
      throw new Error(`VirtualClock cannot move backward (now=${this.#now}, target=${target}).`);
    }

    for (;;) {
      let next: ScheduledTimer | undefined;
      for (const timer of this.#timers.values()) {
        if (timer.dueAt > target) {
          continue;
        }
        if (
          next === undefined ||
          timer.dueAt < next.dueAt ||
          (timer.dueAt === next.dueAt && timer.seq < next.seq)
        ) {
          next = timer;
        }
      }

      if (next === undefined) {
        break;
      }

      this.#timers.delete(next.handle);
      if (next.dueAt > this.#now) {
        this.#now = next.dueAt;
      }
      next.callback();
    }

    this.#now = target;
  }

  /** Convenience: advance virtual time by a relative delta. */
  advance(deltaMs: number): void {
    this.advanceTo(this.#now + deltaMs);
  }
}
