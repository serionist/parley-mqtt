import type { ParleyRuntime, ParleyTimer } from './runtime';

interface Entry {
  timer: ParleyTimer;
}

/**
 * A bounded set of tombstoned command ids (protocol.md C5 / H5 / H8). Each entry self-expires after
 * its TTL, and the table is additionally hard-capped with oldest-first eviction, so state stays
 * bounded on both axes. A `Map` preserves insertion order, giving oldest-first for free.
 */
export class TombstoneTable {
  readonly #runtime: ParleyRuntime;
  readonly #capacity: number;
  readonly #entries = new Map<string, Entry>();

  constructor(runtime: ParleyRuntime, capacity: number) {
    this.#runtime = runtime;
    this.#capacity = capacity;
  }

  get count(): number {
    return this.#entries.size;
  }

  has(id: string): boolean {
    return this.#entries.has(id);
  }

  add(id: string, ttlMs: number): void {
    // Refresh if already present (dispose the old TTL timer and re-insert at the end).
    const existing = this.#entries.get(id);
    if (existing) {
      existing.timer.cancel();
      this.#entries.delete(id);
    }

    const timer = this.#runtime.schedule(ttlMs, () => this.remove(id));
    this.#entries.set(id, { timer });

    // H8: oldest-first eviction when over the hard cap (Map iterates in insertion order).
    while (this.#entries.size > this.#capacity) {
      const oldest = this.#entries.keys().next().value;
      if (oldest === undefined) {
        break;
      }
      this.remove(oldest);
    }
  }

  remove(id: string): void {
    const entry = this.#entries.get(id);
    if (entry) {
      entry.timer.cancel();
      this.#entries.delete(id);
    }
  }

  /** Disposes every entry's TTL timer and empties the table (used on instance disposal). */
  clear(): void {
    for (const entry of this.#entries.values()) {
      entry.timer.cancel();
    }
    this.#entries.clear();
  }
}
