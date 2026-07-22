import {
  ParleyBuiltinError,
  ParleyCommandHandler,
  ParleyOutcome,
  type ParleyConfig,
  type ParleyExecution,
  type ParleyTopics,
  type ParleyClock,
} from '../../src/index';
import type { FakeTransport } from './fakeTransport';

interface Pending {
  readonly resolve: (outcome: ParleyOutcome<unknown, string>) => void;
  readonly execution: ParleyExecution<unknown>;
}

/**
 * A scripted, time-driven handler stub for vector/unit replay. Each admitted command's
 * {@link handleCommand} parks on a promise; the runner resolves it when the scenario's `handler`
 * action fires (`complete`/`fail`/`progress`). Mirrors the C# `VectorHandler`.
 */
export class VectorHandler extends ParleyCommandHandler<unknown, unknown, unknown, string> {
  readonly #pending = new Map<string, Pending>();

  constructor(transport: FakeTransport, topics: ParleyTopics, config: ParleyConfig, clock: ParleyClock) {
    super(transport, topics, config, { clock });
  }

  protected override handleCommand(
    _request: unknown,
    execution: ParleyExecution<unknown>,
  ): Promise<ParleyOutcome<unknown, string>> {
    return new Promise<ParleyOutcome<unknown, string>>((resolve) => {
      this.#pending.set(execution.commandId, { resolve, execution });
    });
  }

  complete(id: string, payload: unknown): void {
    const pending = this.#take(id);
    pending.resolve(ParleyOutcome.success(payload));
  }

  fail(id: string, code: string, message?: string): void {
    const pending = this.#take(id);
    pending.resolve(
      code === ParleyBuiltinError.Cancelled
        ? ParleyOutcome.cancelled()
        : ParleyOutcome.error(code, message),
    );
  }

  emitProgress(id: string, payload: unknown): void {
    this.#peek(id).execution.reportProgress(payload);
  }

  isExecuting(id: string): boolean {
    return this.#pending.has(id);
  }

  isCancelRequested(id: string): boolean {
    return this.#peek(id).execution.signal.aborted;
  }

  #peek(id: string): Pending {
    const pending = this.#pending.get(id);
    if (!pending) {
      throw new Error(`No admitted command '${id}' is executing.`);
    }
    return pending;
  }

  #take(id: string): Pending {
    const pending = this.#peek(id);
    this.#pending.delete(id);
    return pending;
  }
}
