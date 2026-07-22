using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using MQTTnet;
using MQTTnet.Protocol;
using Parley;

namespace Parley.TestPeer
{
    // Real transport adapter over MQTTnet v5 (pinned 5.2.0.1603), publishing/subscribing at a
    // configurable QoS (0 default, 1 for the adapter-compatibility pass) with retain=false — the
    // protocol baseline (protocol.md §2). Parley never holds the connection: the IMqttClient is
    // connected before the adapter sees it. The commander/handler are internally synchronized, so
    // MQTTnet delivering on its own background thread is safe.
    //
    // MQTTnet's raw IMqttClient does NOT auto-reconnect, so this adapter runs its own manual reconnect
    // loop: on an unexpected disconnect it re-issues ConnectAsync (with the stored options) until it
    // succeeds; each successful (re)connection fires MQTTnet's ConnectedAsync, which the adapter
    // forwards to RaiseConnected() so the Parley core re-subscribes every topic (protocol.md §2). The
    // forward is dispatched OFF the ConnectAsync call stack (Task.Run): MQTTnet raises ConnectedAsync
    // synchronously inside ConnectAsync, and the core's connected callback re-subscribes under its gate
    // via SubscribeAsync — calling that inline reenters MQTTnet's connect path and deadlocks while
    // holding the gate, freezing every timer. Detaching it lets ConnectAsync return first.
    //
    // The adapter additionally tracks in-flight SubscribeAsync tasks so the peer can gate its READY
    // line (handler) / suite start (commander) on the broker's SUBACKs — QoS 0 has no retained state,
    // so publishing before a subscription lands would silently drop the message.
    internal sealed class MqttNetAdapter : ParleyConnectionAdapter
    {
        private readonly IMqttClient _client;
        private readonly MqttClientOptions _options;
        private readonly MqttQualityOfServiceLevel _qos;
        private readonly ConcurrentDictionary<string, Action<string, ReadOnlyMemory<byte>>> _handlers = new();
        private readonly List<Task> _pendingSubscribes = new();
        private readonly object _pendingGate = new();
        private volatile bool _shutdown;
        private int _reconnecting;

        public MqttNetAdapter(IMqttClient client, MqttClientOptions options, MqttQualityOfServiceLevel qos)
        {
            _client = client;
            _options = options;
            _qos = qos;
            _client.ApplicationMessageReceivedAsync += OnMessage;
            _client.ConnectedAsync += args =>
            {
                _ = Task.Run(() => RaiseConnected());
                return Task.CompletedTask;
            };
            _client.DisconnectedAsync += OnDisconnected;
        }

        public override void Publish(string topic, ReadOnlyMemory<byte> payload)
        {
            MqttApplicationMessage message = new MqttApplicationMessageBuilder()
                .WithTopic(topic)
                .WithPayload(payload.ToArray())
                .WithQualityOfServiceLevel(_qos)
                .WithRetainFlag(false)
                .Build();
            try
            {
                // Fire-and-forget at the protocol level. Observe (and swallow) any fault so a publish
                // attempted while disconnected drops cleanly — the core reschedules hb/res on its own
                // cadence and re-publishes once reconnected — rather than surfacing as an unobserved fault.
                _ = _client.PublishAsync(message).ContinueWith(
                    static t => { _ = t.Exception; },
                    CancellationToken.None,
                    TaskContinuationOptions.OnlyOnFaulted | TaskContinuationOptions.ExecuteSynchronously,
                    TaskScheduler.Default);
            }
            catch
            {
                // PublishAsync threw synchronously (e.g. not connected) — drop.
            }
        }

        public override void Subscribe(string topic, Action<string, ReadOnlyMemory<byte>> handler)
        {
            _handlers[topic] = handler;
            if (_client.IsConnected)
            {
                Task subscribe = _client.SubscribeAsync(topic, _qos);
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

        // Marks an intentional teardown so the reconnect loop stops fighting a deliberate disconnect.
        public void BeginShutdown() => _shutdown = true;

        private Task OnDisconnected(MqttClientDisconnectedEventArgs e)
        {
            if (!_shutdown && Interlocked.CompareExchange(ref _reconnecting, 1, 0) == 0)
            {
                _ = Task.Run(ReconnectLoopAsync);
            }

            return Task.CompletedTask;
        }

        private async Task ReconnectLoopAsync()
        {
            try
            {
                while (!_shutdown && !_client.IsConnected)
                {
                    try
                    {
                        await Task.Delay(300).ConfigureAwait(false);
                        if (_shutdown)
                        {
                            return;
                        }

                        await _client.ConnectAsync(_options).ConfigureAwait(false);
                        return; // connected → ConnectedAsync fired → library re-subscribed
                    }
                    catch
                    {
                        // broker still down / connect refused — retry until it comes back
                    }
                }
            }
            finally
            {
                Interlocked.Exchange(ref _reconnecting, 0);
            }
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
