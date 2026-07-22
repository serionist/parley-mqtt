// Parley example (C# / .NET) — a `backup` command over MQTT.
// ===========================================================================
//
// Two roles, one command type:
//
//   • the HANDLER runs where the disk lives. It receives a backup request,
//     streams progress while it works, and returns how many bytes it wrote —
//     or a typed error (the disk filled up, the target was unreachable).
//
//   • the COMMANDER runs wherever you start a backup. It calls the command,
//     watches progress, can cancel, and awaits one typed answer.
//
// Both talk over the MQTT broker you already run. Parley never opens the
// connection itself — you hand it a live one through a small adapter (below).
//
//   request   { target: string }        — what to back up
//   progress  { percent: number }       — advisory, streamed while working
//   result    { bytesWritten: number }  — the single terminal success value
//   errors    "diskFull" | "targetUnreachable"  — your own typed error codes

using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using MQTTnet;
using Parley;

namespace Parley.Examples
{
    // -----------------------------------------------------------------------
    // 1. The message shapes for this command.
    // -----------------------------------------------------------------------
    // Ordinary records. Parley binds them with the four generic parameters
    // <Request, Progress, Result, Error>. No schema compiler, no code
    // generation — the source-generated JSON context below supplies the
    // (trim/AOT-friendly) serialization metadata.

    public sealed record BackupRequest(string Target);

    public sealed record BackupProgress(int Percent);

    public sealed record BackupResult(long BytesWritten);

    // Custom error codes are plain strings on the wire; here TError = string.
    // (A custom code must not start with '$' — that prefix is Parley's builtins.)
    public static class BackupErrors
    {
        public const string DiskFull = "diskFull";
        public const string TargetUnreachable = "targetUnreachable";
    }

    // Source-generated JSON metadata for the four payload types. camelCase keeps
    // the wire field names (target, percent, bytesWritten) tidy and portable.
    [JsonSourceGenerationOptions(PropertyNamingPolicy = JsonKnownNamingPolicy.CamelCase)]
    [JsonSerializable(typeof(BackupRequest))]
    [JsonSerializable(typeof(BackupProgress))]
    [JsonSerializable(typeof(BackupResult))]
    [JsonSerializable(typeof(string))] // the custom error code type (TError)
    public partial class BackupJsonContext : JsonSerializerContext
    {
    }

    // =======================================================================
    // 2. An ILLUSTRATIVE transport adapter over MQTTnet.
    // =======================================================================
    // ⚠️  This adapter is hand-written for the example. It has NOT been shipped
    //     as a package or run through CI — it shows the shape you would
    //     implement over your own client. A verified, reusable adapter arrives
    //     with the e2e suite.
    //
    // Parley never connects or holds credentials — the IMqttClient handed in is
    // already connected and authenticated. The commander/handler are internally
    // synchronized, so it is fine for MQTTnet to deliver messages on its own
    // background thread.
    public sealed class MqttNetAdapter : ParleyConnectionAdapter
    {
        private readonly IMqttClient _client;
        private readonly ConcurrentDictionary<string, Action<string, ReadOnlyMemory<byte>>> _handlers = new();

        public MqttNetAdapter(IMqttClient client)
        {
            _client = client;
            _client.ApplicationMessageReceivedAsync += OnMessage;

            // On every (re)connect, tell Parley — it re-subscribes its topics in
            // response, which is how a dropped connection self-heals.
            _client.ConnectedAsync += _ =>
            {
                RaiseConnected();
                return Task.CompletedTask;
            };
        }

        // Fire-and-forget publish at QoS 0 (the builder default), retain=false.
        public override void Publish(string topic, ReadOnlyMemory<byte> payload)
        {
            var message = new MqttApplicationMessageBuilder()
                .WithTopic(topic)
                .WithPayload(payload.ToArray())
                .WithRetainFlag(false)
                .Build();
            _ = _client.PublishAsync(message);
        }

        // Replace-semantics: one handler per topic. Record it, and if we are
        // already connected, subscribe at the broker now.
        public override void Subscribe(string topic, Action<string, ReadOnlyMemory<byte>> handler)
        {
            _handlers[topic] = handler;
            if (_client.IsConnected)
            {
                _ = _client.SubscribeAsync(topic);
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

        private Task OnMessage(MqttApplicationMessageReceivedEventArgs e)
        {
            string topic = e.ApplicationMessage.Topic;
            if (_handlers.TryGetValue(topic, out var handler))
            {
                // MQTTnet v5 exposes the payload as a ReadOnlySequence<byte>.
                handler(topic, e.ApplicationMessage.Payload.ToArray());
            }

            return Task.CompletedTask;
        }
    }

    // =======================================================================
    // 3. The HANDLER — the side that performs the backup.
    // =======================================================================
    // Subclass ParleyCommandHandler and implement HandleCommandAsync. Expected
    // failures are RETURNED as ParleyOutcome values, never thrown — throwing is
    // reserved for genuine bugs (which Parley contains: the command aborts,
    // nothing leaks).
    public sealed class BackupHandler
        : ParleyCommandHandler<BackupRequest, BackupProgress, BackupResult, string>
    {
        public BackupHandler(ParleyConnectionAdapter adapter, ParleyTopics topics, ParleyConfig config)
            : base(adapter, topics, config, BackupJsonContext.Default)
        {
        }

        protected override async Task<ParleyOutcome<BackupResult, string>> HandleCommandAsync(
            BackupRequest request,
            IParleyExecution<BackupProgress> execution,
            CancellationToken cancellationToken)
        {
            // A made-up backup that "copies" ten chunks. While it runs, Parley is
            // heartbeating this command every few seconds on your behalf, so the
            // caller can always tell "slow" from "dead" — you never fake progress
            // just to stay trusted.
            long bytesWritten = 0;

            for (int chunk = 1; chunk <= 10; chunk++)
            {
                // Cooperative cancellation: a wire-level `cnl` from the caller
                // signals this token. Observe it and stop cleanly.
                if (cancellationToken.IsCancellationRequested)
                {
                    return ParleyOutcome.Cancelled(); // → the caller sees $cancelled
                }

                long wrote = await CopyOneChunkAsync(request.Target, chunk).ConfigureAwait(false);
                if (wrote < 0)
                {
                    // Domain failures are typed values — returned to the caller typed.
                    return ParleyOutcome.Error(BackupErrors.DiskFull, "no space left on device");
                }

                bytesWritten += wrote;

                // Advisory progress. Each call publishes one `prg`; delivery is
                // best-effort (never acknowledged), so it is safe to send often.
                execution.ReportProgress(new BackupProgress(chunk * 10));
            }

            // The single terminal success value.
            return ParleyOutcome.Success(new BackupResult(bytesWritten));
        }

        private static async Task<long> CopyOneChunkAsync(string target, int chunk)
        {
            await Task.Delay(200).ConfigureAwait(false);
            return 1024L * chunk; // pretend each chunk is a little bigger
        }
    }

    // =======================================================================
    // 4. The COMMANDER — the side that requests a backup.
    // =======================================================================
    public static class BackupCaller
    {
        public static async Task RequestBackupAsync(
            ParleyCommander<BackupRequest, BackupProgress, BackupResult, string> commander)
        {
            // A CancellationToken is a valid cancel signal. Cancel it any time
            // before the result lands and Parley sends a real `cnl` to the handler.
            using var cts = new CancellationTokenSource();

            // Auto-cancel this backup if it somehow runs longer than five minutes.
            cts.CancelAfter(TimeSpan.FromMinutes(5));

            // Per-call options: also cap how long we wait once the handler is
            // alive (protocol.md §8). Anything left null inherits the config.
            var options = new ParleyCallOptions
            {
                ExecutionTimeout = TimeSpan.FromMinutes(5),
            };

            // A Progress<T> sink receives the advisory progress updates.
            var progress = new Progress<BackupProgress>(p => Console.WriteLine($"backup {p.Percent}% done"));

            ParleyResult<BackupResult, string> result =
                await commander.ExecuteAsync(new BackupRequest("photos"), options, progress, cts.Token)
                    .ConfigureAwait(false);

            // Every outcome — success, your errors, and all six builtins — is a value.
            if (result.IsSuccess)
            {
                Console.WriteLine($"backup complete: {result.Value!.BytesWritten} bytes written");
                return;
            }

            ParleyError<string> error = result.Error;
            if (error.IsBuiltin)
            {
                // A builtin: something about delivery/liveness, not your domain.
                switch (error.Builtin)
                {
                    case ParleyBuiltinError.NoAck:
                        Console.Error.WriteLine("no handler answered — is the device online?");
                        break;
                    case ParleyBuiltinError.Busy:
                        Console.Error.WriteLine("the device is at capacity — retry shortly");
                        break;
                    case ParleyBuiltinError.ExecutionStale:
                        Console.Error.WriteLine("the device went silent mid-backup");
                        break;
                    case ParleyBuiltinError.ExecutionTimeout:
                        Console.Error.WriteLine("the backup ran past its time cap");
                        break;
                    case ParleyBuiltinError.Cancelled:
                        Console.Error.WriteLine("the backup was cancelled");
                        break;
                    case ParleyBuiltinError.DecodeError:
                        Console.Error.WriteLine("the device sent a result we could not read");
                        break;
                }
            }
            else
            {
                // A custom code — here TError is string, so switch on error.Custom.
                switch (error.Custom)
                {
                    case BackupErrors.DiskFull:
                        Console.Error.WriteLine($"out of space: {error.Message}");
                        break;
                    case BackupErrors.TargetUnreachable:
                        Console.Error.WriteLine("the backup target could not be reached");
                        break;
                }
            }
        }
    }

    // =======================================================================
    // 5. Wiring it up.
    // =======================================================================
    public static class Program
    {
        private static readonly ParleyTopics BackupTopics = new(
            request: "home/backup/req", //  commander → handler: req, rak, cnl
            heartbeat: "home/backup/hb", // handler → commander: heartbeats
            progress: "home/backup/prg", // handler → commander: progress
            result: "home/backup/res"); //  handler → commander: the result

        public static async Task Main(string[] args)
        {
            string role = args.Length > 0 ? args[0] : "commander";

            // YOU own the connection: server, credentials, and TLS all live here,
            // never in Parley. Connect before handing the client to the adapter.
            var factory = new MqttClientFactory();
            IMqttClient client = factory.CreateMqttClient();
            var mqttOptions = new MqttClientOptionsBuilder()
                .WithTcpServer("localhost", 1883)
                // .WithCredentials(...) / .WithTlsOptions(...) go here — Parley never sees them.
                .Build();
            await client.ConnectAsync(mqttOptions).ConfigureAwait(false);

            var adapter = new MqttNetAdapter(client);
            var config = new ParleyConfig();

            if (role == "handler")
            {
                using var handler = new BackupHandler(adapter, BackupTopics, config);
                Console.WriteLine("backup handler ready");
                await Task.Delay(Timeout.Infinite).ConfigureAwait(false); // serve until shutdown
            }
            else
            {
                using var commander =
                    new ParleyCommander<BackupRequest, BackupProgress, BackupResult, string>(
                        adapter, BackupTopics, config, BackupJsonContext.Default);
                await BackupCaller.RequestBackupAsync(commander).ConfigureAwait(false);
            }
        }
    }
}
