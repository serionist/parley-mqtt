using System;
using System.Collections.Generic;
using Parley;

namespace Parley.Tests.Infrastructure;

/// <summary>One recorded outbound action: a publish (topic + payload) or a subscribe (topic).</summary>
internal sealed class OutboundAction
{
    public bool IsSubscribe { get; private init; }

    public string Topic { get; private init; } = string.Empty;

    public byte[]? Payload { get; private init; }

    public static OutboundAction Publish(string topic, byte[] payload) =>
        new() { IsSubscribe = false, Topic = topic, Payload = payload };

    public static OutboundAction Subscribe(string topic) =>
        new() { IsSubscribe = true, Topic = topic };
}

/// <summary>
/// A fake in-memory transport: records every publish and subscribe, delivers scripted inbound messages
/// to the library's per-topic handlers, and can fire the connected signal. No broker, no threads.
/// </summary>
internal sealed class FakeTransport : ParleyConnectionAdapter
{
    private readonly Dictionary<string, Action<string, ReadOnlyMemory<byte>>> _handlers = new();

    public List<OutboundAction> Outbound { get; } = new();

    /// <summary>When set, <see cref="Publish"/> throws — simulates a disconnected/disposed underlying client
    /// so tests can prove adapter faults never escape onto Parley-owned timer threads (CR4).</summary>
    public bool FailPublish { get; set; }

    public override void Publish(string topic, ReadOnlyMemory<byte> payload)
    {
        if (FailPublish)
        {
            throw new InvalidOperationException("FakeTransport: publish failed (simulated adapter fault).");
        }

        Outbound.Add(OutboundAction.Publish(topic, payload.ToArray()));
    }

    public override void Subscribe(string topic, Action<string, ReadOnlyMemory<byte>> handler)
    {
        _handlers[topic] = handler;
        Outbound.Add(OutboundAction.Subscribe(topic));
    }

    public override void Unsubscribe(string topic) => _handlers.Remove(topic);

    /// <summary>Delivers a raw payload to the handler subscribed on <paramref name="topic"/>, if any.</summary>
    public void Deliver(string topic, byte[] payload)
    {
        if (_handlers.TryGetValue(topic, out Action<string, ReadOnlyMemory<byte>>? handler))
        {
            handler(topic, payload);
        }
    }

    /// <summary>Fires the transport-connected signal (a reconnection).</summary>
    public void FireConnected() => RaiseConnected();

    /// <summary>Discards recorded outbound actions (used to exclude construction-time subscribes).</summary>
    public void ClearOutbound() => Outbound.Clear();
}
