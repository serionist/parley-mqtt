import { ParleyConnectionAdapter, type ParleyMessageHandler } from '../../src/index';

/** One recorded outbound action: a publish (topic + payload) or a subscribe (topic). */
export type OutboundAction =
  | { readonly kind: 'publish'; readonly topic: string; readonly payload: Uint8Array }
  | { readonly kind: 'subscribe'; readonly topic: string };

/**
 * A fake in-memory transport: records every publish and subscribe, delivers scripted inbound
 * messages to the library's per-topic handlers, and can fire the connected signal. No broker.
 */
export class FakeTransport extends ParleyConnectionAdapter {
  readonly #handlers = new Map<string, ParleyMessageHandler>();
  outbound: OutboundAction[] = [];

  /** When set, {@link publish} throws — simulates a disconnected/disposed client (containment tests). */
  failPublish = false;

  override publish(topic: string, payload: Uint8Array): void {
    if (this.failPublish) {
      throw new Error('FakeTransport: publish failed (simulated adapter fault).');
    }
    this.outbound.push({ kind: 'publish', topic, payload: payload.slice() });
  }

  override subscribe(topic: string, onMessage: ParleyMessageHandler): void {
    this.#handlers.set(topic, onMessage);
    this.outbound.push({ kind: 'subscribe', topic });
  }

  override unsubscribe(topic: string): void {
    this.#handlers.delete(topic);
  }

  /** Delivers a raw payload to the handler subscribed on `topic`, if any. */
  deliver(topic: string, payload: Uint8Array): void {
    this.#handlers.get(topic)?.(topic, payload);
  }

  /** Fires the transport-connected signal (a reconnection). */
  fireConnected(): void {
    this.raiseConnected();
  }

  /** Discards recorded outbound actions (used to exclude construction-time subscribes). */
  clearOutbound(): void {
    this.outbound = [];
  }

  /** Convenience: parsed publish payloads on a topic. */
  publishes(topic: string): Array<Record<string, unknown>> {
    const decoder = new TextDecoder();
    return this.outbound
      .filter((a): a is Extract<OutboundAction, { kind: 'publish' }> => a.kind === 'publish' && a.topic === topic)
      .map((a) => JSON.parse(decoder.decode(a.payload)) as Record<string, unknown>);
  }

  /** Convenience: count of publishes of a given message type on a topic. */
  publishCount(topic: string, type: string): number {
    return this.publishes(topic).filter((m) => m.t === type).length;
  }

  /** Convenience: count of subscribes recorded for a topic. */
  subscribeCount(topic: string): number {
    return this.outbound.filter((a) => a.kind === 'subscribe' && a.topic === topic).length;
  }
}
