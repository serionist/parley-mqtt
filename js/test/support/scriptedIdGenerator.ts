import type { ParleyIdGenerator } from '../../src/index';

/**
 * A command-id generator that returns scripted ids in FIFO order, so vector/unit runners bypass the
 * random §4.2 generator and use the ids the scenario specifies (protocol.md §10).
 */
export class ScriptedIdGenerator implements ParleyIdGenerator {
  readonly #ids: string[] = [];

  enqueue(id: string): void {
    this.#ids.push(id);
  }

  newId(): string {
    const id = this.#ids.shift();
    if (id === undefined) {
      throw new Error('No scripted command id available.');
    }
    return id;
  }
}
