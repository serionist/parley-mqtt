using System;

namespace Parley
{
    /// <summary>
    /// The transport port the consumer implements over their own MQTT client. Parley never connects,
    /// authenticates, or configures TLS; it is handed a live session through this adapter (protocol.md §2).
    /// All publishes MUST use <c>retain = false</c> and MAY be QoS 0 or 1 — the protocol is correct at QoS 0.
    /// </summary>
    public abstract class ParleyConnectionAdapter
    {
        /// <summary>Raised on every (re)connection. The library re-subscribes its topics in response.</summary>
        public event Action? Connected;

        /// <summary>Publishes a UTF-8 JSON payload to a topic (fire-and-forget, <c>retain = false</c>).</summary>
        /// <param name="topic">The destination topic.</param>
        /// <param name="payload">The UTF-8 JSON payload bytes.</param>
        public abstract void Publish(string topic, ReadOnlyMemory<byte> payload);

        /// <summary>Subscribes to a topic. Inbound messages are delivered to <paramref name="handler"/>.</summary>
        /// <param name="topic">The topic to subscribe to.</param>
        /// <param name="handler">Invoked as <c>handler(topic, payloadBytes)</c> for each inbound message.</param>
        public abstract void Subscribe(string topic, Action<string, ReadOnlyMemory<byte>> handler);

        /// <summary>Unsubscribes from a topic.</summary>
        /// <param name="topic">The topic to unsubscribe from.</param>
        public abstract void Unsubscribe(string topic);

        /// <summary>Raises <see cref="Connected"/>. Adapter implementations call this on every (re)connection.</summary>
        protected void RaiseConnected() => Connected?.Invoke();
    }
}
