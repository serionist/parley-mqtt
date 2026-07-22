using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Threading.Tasks;
using MQTTnet;
using Parley;

namespace Parley.TestPeer
{
    // Real transport adapter over MQTTnet v5 (pinned 5.2.0.1603), publishing/subscribing at QoS 0
    // with retain=false — the protocol baseline (protocol.md §2). Parley never holds the connection:
    // the IMqttClient is connected before the adapter sees it. The commander/handler are internally
    // synchronized, so MQTTnet delivering on its own background thread is safe.
    //
    // The adapter additionally tracks in-flight SubscribeAsync tasks so the peer can gate its READY
    // line (handler) / suite start (commander) on the broker's SUBACKs — QoS 0 has no retained state,
    // so publishing before a subscription lands would silently drop the message.
    internal sealed class MqttNetAdapter : ParleyConnectionAdapter
    {
        private readonly IMqttClient _client;
        private readonly ConcurrentDictionary<string, Action<string, ReadOnlyMemory<byte>>> _handlers = new();
        private readonly List<Task> _pendingSubscribes = new();
        private readonly object _pendingGate = new();

        public MqttNetAdapter(IMqttClient client)
        {
            _client = client;
            _client.ApplicationMessageReceivedAsync += OnMessage;
            _client.ConnectedAsync += _ =>
            {
                RaiseConnected();
                return Task.CompletedTask;
            };
        }

        public override void Publish(string topic, ReadOnlyMemory<byte> payload)
        {
            MqttApplicationMessage message = new MqttApplicationMessageBuilder()
                .WithTopic(topic)
                .WithPayload(payload.ToArray())
                .WithQualityOfServiceLevel(MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce)
                .WithRetainFlag(false)
                .Build();
            _ = _client.PublishAsync(message);
        }

        public override void Subscribe(string topic, Action<string, ReadOnlyMemory<byte>> handler)
        {
            _handlers[topic] = handler;
            if (_client.IsConnected)
            {
                Task subscribe = _client.SubscribeAsync(topic, MQTTnet.Protocol.MqttQualityOfServiceLevel.AtMostOnce);
                lock (_pendingGate)
                {
                    _pendingSubscribes.Add(subscribe);
                }
            }
        }

        public override void Unsubscribe(string topic)
        {
            _handlers.TryRemove(topic, out _);
            if (_client.IsConnected)
            {
                _ = _client.UnsubscribeAsync(topic);
            }
        }

        // Awaits every SubscribeAsync issued so far — i.e. all SUBACKs have landed at the broker.
        public Task WaitForSubscriptionsAsync()
        {
            Task[] snapshot;
            lock (_pendingGate)
            {
                snapshot = _pendingSubscribes.ToArray();
            }

            return Task.WhenAll(snapshot);
        }

        private Task OnMessage(MqttApplicationMessageReceivedEventArgs e)
        {
            string topic = e.ApplicationMessage.Topic;
            if (_handlers.TryGetValue(topic, out Action<string, ReadOnlyMemory<byte>>? handler))
            {
                // MQTTnet v5 exposes the payload as a ReadOnlySequence<byte>.
                handler(topic, e.ApplicationMessage.Payload.ToArray());
            }

            return Task.CompletedTask;
        }
    }
}
