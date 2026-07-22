/** Delivered inbound payload callback: `(topic, payloadBytes)`. */
export type ParleyMessageHandler = (topic: string, payload: Uint8Array) => void;

/**
 * The transport port the consumer implements over their own MQTT client (design.md). Parley never
 * connects, authenticates, or configures TLS; it is handed a live session (protocol.md §2). All
 * publishes MUST use `retain = false` and MAY be QoS 0 or 1 — the protocol is correct at QoS 0.
 *
 * `subscribe` has **replace** semantics: subscribing a topic replaces any prior handler for it (one
 * handler per topic). The library re-subscribes every topic on each connected signal without
 * unsubscribing first, so an appending adapter would multiply-deliver.
 */
export abstract class ParleyConnectionAdapter {
  readonly #connectedCallbacks = new Set<() => void>();

  /** Publishes a UTF-8 JSON payload to a topic (fire-and-forget, `retain = false`). */
  abstract publish(topic: string, payload: Uint8Array): void;

  /** Subscribes to a topic; replaces any prior handler for it. */
  abstract subscribe(topic: string, onMessage: ParleyMessageHandler): void;

  /** Unsubscribes from a topic. */
  abstract unsubscribe(topic: string): void;

  /** Registers a callback fired on every (re)connection. The library re-subscribes in response. */
  onConnected(callback: () => void): void {
    this.#connectedCallbacks.add(callback);
  }

  /** Detaches a previously registered connected callback. */
  offConnected(callback: () => void): void {
    this.#connectedCallbacks.delete(callback);
  }

  /** Adapter implementations call this on every (re)connection. */
  protected raiseConnected(): void {
    for (const callback of [...this.#connectedCallbacks]) {
      callback();
    }
  }
}
