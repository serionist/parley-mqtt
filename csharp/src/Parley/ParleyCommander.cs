using System;
using System.Collections.Generic;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;
using System.Threading;
using System.Threading.Tasks;
using Parley.Internal;

namespace Parley
{
    /// <summary>
    /// The caller side of one command type (protocol.md §6, rules C1–C8). Generic over the request,
    /// progress, result, and custom-error types. Multiple commanders MAY share a topic set; each filters
    /// inbound traffic by command id. Expected outcomes — including all five builtin codes — are returned
    /// as <see cref="ParleyResult{TResult,TError}"/> values, never thrown.
    /// </summary>
    /// <typeparam name="TRequest">The request payload type.</typeparam>
    /// <typeparam name="TProgress">The progress payload type.</typeparam>
    /// <typeparam name="TResult">The result payload type.</typeparam>
    /// <typeparam name="TError">The consumer's custom error-code type.</typeparam>
    public sealed class ParleyCommander<TRequest, TProgress, TResult, TError> : IDisposable
    {
        private readonly ParleyConnectionAdapter _adapter;
        private readonly ParleyTopics _topics;
        private readonly ParleyConfig _config;
        private readonly ParleyRuntime _runtime;
        private readonly IParleyCommandIdGenerator _idGenerator;
        private readonly JsonTypeInfo<TRequest> _requestTypeInfo;
        private readonly JsonTypeInfo<TProgress> _progressTypeInfo;
        private readonly JsonTypeInfo<TResult> _resultTypeInfo;
        private readonly JsonTypeInfo<TError> _errorTypeInfo;
        private readonly Dictionary<string, Command> _inflight = new();
        private readonly TombstoneTable _tombstones;
        private bool _disposed;

        /// <summary>Creates a commander for one command type.</summary>
        /// <param name="adapter">The transport adapter.</param>
        /// <param name="topics">The four topic strings for this command type.</param>
        /// <param name="config">Timing configuration.</param>
        /// <param name="jsonContext">A source-generated context providing type info for the four payload types.</param>
        /// <param name="idGenerator">Command-id source; defaults to a cryptographically random generator.</param>
        /// <param name="timeProvider">Clock; defaults to <see cref="TimeProvider.System"/>.</param>
        /// <exception cref="ArgumentNullException">A required argument is null.</exception>
        /// <exception cref="ArgumentException">The config is invalid or a payload type has no source-gen metadata.</exception>
        public ParleyCommander(
            ParleyConnectionAdapter adapter,
            ParleyTopics topics,
            ParleyConfig config,
            JsonSerializerContext jsonContext,
            IParleyCommandIdGenerator? idGenerator = null,
            TimeProvider? timeProvider = null)
        {
            _adapter = adapter ?? throw new ArgumentNullException(nameof(adapter));
            _topics = topics ?? throw new ArgumentNullException(nameof(topics));
            _config = config ?? throw new ArgumentNullException(nameof(config));
            if (jsonContext is null)
            {
                throw new ArgumentNullException(nameof(jsonContext));
            }

            _config.Validate();
            _idGenerator = idGenerator ?? ParleyRandomCommandIdGenerator.Instance;
            _runtime = new ParleyRuntime(timeProvider ?? TimeProvider.System);
            _tombstones = new TombstoneTable(_runtime, _config.MaxTombstones);

            _requestTypeInfo = TypeInfoResolver.Resolve<TRequest>(jsonContext);
            _progressTypeInfo = TypeInfoResolver.Resolve<TProgress>(jsonContext);
            _resultTypeInfo = TypeInfoResolver.Resolve<TResult>(jsonContext);
            _errorTypeInfo = TypeInfoResolver.Resolve<TError>(jsonContext);

            _adapter.Connected += OnConnected;
            SubscribeAll();
        }

        private void SubscribeAll()
        {
            _adapter.Subscribe(_topics.Heartbeat, (_, payload) => OnInbound(payload, MessageTypes.Heartbeat));
            _adapter.Subscribe(_topics.Progress, (_, payload) => OnInbound(payload, MessageTypes.Progress));
            _adapter.Subscribe(_topics.Result, (_, payload) => OnInbound(payload, MessageTypes.Result));
        }

        private void OnConnected()
        {
            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    return;
                }

                SubscribeAll();
            }
        }

        /// <summary>
        /// Executes one command and awaits its single terminal outcome (protocol.md C1–C5).
        /// Cancelling <paramref name="cancellationToken"/> triggers a wire-level <c>cnl</c> (C7); the
        /// returned result is still whatever the command actually reaches (a genuine result may win the race).
        /// </summary>
        /// <param name="request">The request payload.</param>
        /// <param name="options">Per-call timing overrides; null uses the configured defaults.</param>
        /// <param name="progress">Optional progress sink for advisory <c>prg</c> updates (C8).</param>
        /// <param name="cancellationToken">Cancels the call at the wire level (C7).</param>
        /// <returns>The terminal outcome as a value.</returns>
        public Task<ParleyResult<TResult, TError>> ExecuteAsync(
            TRequest request,
            ParleyCallOptions? options = null,
            IProgress<TProgress>? progress = null,
            CancellationToken cancellationToken = default)
        {
            var tcs = new TaskCompletionSource<ParleyResult<TResult, TError>>(TaskCreationOptions.RunContinuationsAsynchronously);

            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    throw new ObjectDisposedException("ParleyCommander");
                }

                string id = _idGenerator.NewCommandId();
                if (!MessageCodec.IsValidId(id))
                {
                    throw new InvalidOperationException($"Command-id generator produced an invalid id: '{id}'.");
                }

                if (_inflight.ContainsKey(id) || _tombstones.Contains(id))
                {
                    throw new InvalidOperationException($"Command id '{id}' collides with existing state.");
                }

                var cmd = new Command(id, tcs, progress, ResolveOptions(options));
                JsonElement? payload = MessageCodec.ToPayloadElement(request, _requestTypeInfo);
                cmd.RequestBytes = MessageCodec.Serialize(new Envelope { V = MessageCodec.Version, T = MessageTypes.Request, Id = id, P = payload });
                _inflight[id] = cmd;

                Publish(_topics.Request, cmd.RequestBytes);
                cmd.RequestRetryTimer = _runtime.Schedule(cmd.Options.RequestRetryInterval, () => OnRequestRetry(cmd));
                cmd.SignOfLifeTimer = _runtime.Schedule(cmd.Options.NoAckTimeout, () => OnSignOfLifeExpiry(cmd));

                if (cancellationToken.CanBeCanceled)
                {
                    cmd.CancellationRegistration = cancellationToken.Register(
                        static state =>
                        {
                            var (self, commandId) = ((ParleyCommander<TRequest, TProgress, TResult, TError>, string))state!;
                            self.RequestCancel(commandId);
                        },
                        (this, id));
                }
            }

            return tcs.Task;
        }

        private void RequestCancel(string id)
        {
            lock (_runtime.Gate)
            {
                if (_disposed || !_inflight.TryGetValue(id, out Command? cmd) || cmd.Cancelling)
                {
                    return;
                }

                cmd.Cancelling = true;

                // C7: a command the caller no longer wants must not keep being offered for admission.
                if (cmd.State == CommandState.Sending)
                {
                    cmd.RequestRetryTimer?.Dispose();
                    cmd.RequestRetryTimer = null;
                }

                cmd.CancelBytes = MessageCodec.Serialize(new Envelope { V = MessageCodec.Version, T = MessageTypes.Cancel, Id = id });
                Publish(_topics.Request, cmd.CancelBytes);
                cmd.CancelRetryTimer = _runtime.Schedule(cmd.Options.CancelRetryInterval, () => OnCancelRetry(cmd));
            }
        }

        private void OnRequestRetry(Command cmd)
        {
            // Only armed while SENDING. Reschedule before publishing so a throwing adapter (CR4) never kills the
            // cadence — the re-send self-heals once the adapter recovers.
            cmd.RequestRetryTimer!.Reschedule(cmd.Options.RequestRetryInterval);
            Publish(_topics.Request, cmd.RequestBytes!);
        }

        private void OnCancelRetry(Command cmd)
        {
            // Reschedule before publishing (see OnRequestRetry / CR4).
            cmd.CancelRetryTimer!.Reschedule(cmd.Options.CancelRetryInterval);
            Publish(_topics.Request, cmd.CancelBytes!);
        }

        private void OnSignOfLifeExpiry(Command cmd)
        {
            // SENDING → $noAck (never any sign of life); LIVE → $executionStale (went silent).
            if (cmd.State == CommandState.Sending)
            {
                TerminateBuiltin(cmd, ParleyBuiltinError.NoAck);
            }
            else
            {
                TerminateBuiltin(cmd, ParleyBuiltinError.ExecutionStale);
            }
        }

        private void OnExecutionCapExpiry(Command cmd)
        {
            TerminateBuiltin(cmd, ParleyBuiltinError.ExecutionTimeout);
        }

        private void OnInbound(ReadOnlyMemory<byte> payload, string expectedType)
        {
            Envelope? env = MessageCodec.TryParse(payload);
            if (env is null
                || env.V != MessageCodec.Version
                || env.T != expectedType
                || !MessageCodec.IsValidId(env.Id))
            {
                return;
            }

            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    return;
                }

                switch (expectedType)
                {
                    case MessageTypes.Heartbeat:
                        OnHeartbeat(env.Id!);
                        break;
                    case MessageTypes.Progress:
                        OnProgress(env.Id!, env.P);
                        break;
                    case MessageTypes.Result:
                        OnResult(env);
                        break;
                }
            }
        }

        private void OnHeartbeat(string id)
        {
            if (_inflight.TryGetValue(id, out Command? cmd))
            {
                OnSignOfLife(cmd);
            }
        }

        private void OnProgress(string id, JsonElement? payload)
        {
            if (!_inflight.TryGetValue(id, out Command? cmd))
            {
                return;
            }

            OnSignOfLife(cmd);

            if (cmd.Progress is { } sink)
            {
                TProgress? value;
                try
                {
                    value = payload.HasValue ? MessageCodec.FromElement(payload.Value, _progressTypeInfo) : default;
                }
                catch (Exception ex) when (ex is JsonException or NotSupportedException)
                {
                    // C8: progress delivery is advisory — a prg whose payload does not decode to TProgress is
                    // dropped silently (the message still counted as a sign of life above).
                    return;
                }

                sink.Report(value!);
            }
        }

        private void OnResult(Envelope env)
        {
            string id = env.Id!;

            // ok is required on res; e is required when ok:false (validated per §4).
            if (env.Ok is not bool ok || (ok == false && env.E is null))
            {
                return;
            }

            bool isBusy = ok == false && env.E == ParleyErrorCodes.Busy;

            if (_inflight.TryGetValue(id, out Command? cmd))
            {
                // C4 exception: a $busy res in LIVE can only be a stale pre-admission reply — ignore it.
                if (cmd.State == CommandState.Live && isBusy)
                {
                    return;
                }

                if (ok)
                {
                    // C4: deserialize the payload BEFORE acking. A success payload that fails to decode to TResult
                    // is a peer contract mismatch — still publish rak (the result WAS delivered; redelivery cannot
                    // fix a mismatch) and terminate locally with the commander-local builtin $decodeError.
                    TResult? value = default;
                    bool decoded = true;
                    try
                    {
                        if (env.P.HasValue)
                        {
                            value = MessageCodec.FromElement(env.P.Value, _resultTypeInfo);
                        }
                    }
                    catch (Exception ex) when (ex is JsonException or NotSupportedException)
                    {
                        decoded = false;
                    }

                    PublishResultAck(id);

                    if (decoded)
                    {
                        CompleteSuccess(cmd, value);
                    }
                    else
                    {
                        TerminateBuiltin(cmd, ParleyBuiltinError.DecodeError);
                    }
                }
                else
                {
                    // C4: rak every terminal error res except $busy (the handler holds no state for a refused id).
                    if (!isBusy)
                    {
                        PublishResultAck(id);
                    }

                    CompleteError(cmd, BuildError(env.E!, env.M));
                }

                return;
            }

            // C5: a res for a tombstoned id re-emits rak (unless $busy) and discards the content.
            if (_tombstones.Contains(id))
            {
                if (!isBusy)
                {
                    PublishResultAck(id);
                }

                return;
            }

            // C6: neither in-flight nor tombstoned — belongs to another commander instance. Ignore.
        }

        private void OnSignOfLife(Command cmd)
        {
            if (cmd.State == CommandState.Sending)
            {
                // C2: first sign of life — enter LIVE, stop req retries, start the execution cap.
                cmd.State = CommandState.Live;
                cmd.RequestRetryTimer?.Dispose();
                cmd.RequestRetryTimer = null;

                if (cmd.Options.ExecutionTimeout is { } cap)
                {
                    cmd.ExecutionCapTimer = _runtime.Schedule(cap, () => OnExecutionCapExpiry(cmd));
                }
            }

            // The sliding sign-of-life timer restarts on every sign of life (C2/C3).
            cmd.SignOfLifeTimer!.Reschedule(cmd.Options.StaleTimeout);
        }

        private ParleyError<TError> BuildError(string code, string? message)
        {
            if (ParleyErrorCodes.TryParse(code, out ParleyBuiltinError builtin))
            {
                return new ParleyError<TError>(code, builtin, default, message, isBuiltin: true);
            }

            // Custom errors derive from the e string code only (structured error p is not surfaced in v1).
            TError? custom = ErrorCodec.WireToCustom(code, _errorTypeInfo);
            return new ParleyError<TError>(code, default, custom, message, isBuiltin: false);
        }

        private void CompleteSuccess(Command cmd, TResult? value)
        {
            Teardown(cmd);
            cmd.CompletionSource.TrySetResult(ParleyResult<TResult, TError>.Success(value));
        }

        private void CompleteError(Command cmd, ParleyError<TError> error)
        {
            Teardown(cmd);
            cmd.CompletionSource.TrySetResult(ParleyResult<TResult, TError>.Failure(error));
        }

        private void TerminateBuiltin(Command cmd, ParleyBuiltinError builtin)
        {
            var error = new ParleyError<TError>(ParleyErrorCodes.ToWire(builtin), builtin, default, null, isBuiltin: true);
            CompleteError(cmd, error);
        }

        private void Teardown(Command cmd)
        {
            cmd.RequestRetryTimer?.Dispose();
            cmd.SignOfLifeTimer?.Dispose();
            cmd.ExecutionCapTimer?.Dispose();
            cmd.CancelRetryTimer?.Dispose();
            cmd.RequestRetryTimer = null;
            cmd.SignOfLifeTimer = null;
            cmd.ExecutionCapTimer = null;
            cmd.CancelRetryTimer = null;
            ReleaseCancellationRegistration(cmd.CancellationRegistration);

            _inflight.Remove(cmd.Id);
            // C5: every terminal outcome tombstones the id for tombstoneTtl.
            _tombstones.Add(cmd.Id, cmd.Options.TombstoneTtl);
        }

        // CR1: never block on the CancellationTokenRegistration while holding the gate. Dispose() waits for a
        // concurrently-running RequestCancel callback, which needs the same gate → deadlock and a lost completion.
        // net10.0 Unregister() is non-blocking; on netstandard2.0 (no Unregister) defer Dispose to the threadpool —
        // RequestCancel is idempotent, so a late stale callback is harmless.
        private static void ReleaseCancellationRegistration(CancellationTokenRegistration registration)
        {
#if NET10_0_OR_GREATER
            registration.Unregister();
#else
            ThreadPool.QueueUserWorkItem(static state => ((CancellationTokenRegistration)state!).Dispose(), registration);
#endif
        }

        private void PublishResultAck(string id)
        {
            byte[] bytes = MessageCodec.Serialize(new Envelope { V = MessageCodec.Version, T = MessageTypes.ResultAck, Id = id });
            Publish(_topics.Request, bytes);
        }

        private void Publish(string topic, byte[] bytes) => _adapter.Publish(topic, bytes);

        /// <summary>
        /// Disposes the commander (design.md): detaches the connected callback, unsubscribes the library's topics,
        /// disposes every per-command timer/registration, completes every in-flight call locally with <c>$cancelled</c>,
        /// and clears all state. Idempotent. After disposal, <see cref="ExecuteAsync"/> throws
        /// <see cref="ObjectDisposedException"/> and inbound transport callbacks are ignored.
        /// </summary>
        public void Dispose()
        {
            var registrations = new List<CancellationTokenRegistration>();

            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    return;
                }

                _disposed = true;

                _adapter.Connected -= OnConnected;
                _adapter.Unsubscribe(_topics.Heartbeat);
                _adapter.Unsubscribe(_topics.Progress);
                _adapter.Unsubscribe(_topics.Result);

                var cancelled = ParleyResult<TResult, TError>.Failure(
                    new ParleyError<TError>(ParleyErrorCodes.Cancelled, ParleyBuiltinError.Cancelled, default, null, isBuiltin: true));

                foreach (Command cmd in _inflight.Values)
                {
                    cmd.RequestRetryTimer?.Dispose();
                    cmd.SignOfLifeTimer?.Dispose();
                    cmd.ExecutionCapTimer?.Dispose();
                    cmd.CancelRetryTimer?.Dispose();
                    registrations.Add(cmd.CancellationRegistration);
                    cmd.CompletionSource.TrySetResult(cancelled);
                }

                _inflight.Clear();
                _tombstones.Clear();
            }

            // CR1: release registrations outside the gate (Dispose() blocks on a concurrent RequestCancel callback).
            foreach (CancellationTokenRegistration registration in registrations)
            {
                ReleaseCancellationRegistration(registration);
            }
        }

        private CommandOptions ResolveOptions(ParleyCallOptions? options)
        {
            TimeSpan? executionTimeout = options?.DisableExecutionTimeout == true
                ? null
                : options?.ExecutionTimeout ?? _config.ExecutionTimeout;

            return new CommandOptions
            {
                RequestRetryInterval = options?.RequestRetryInterval ?? _config.RequestRetryInterval,
                NoAckTimeout = options?.NoAckTimeout ?? _config.NoAckTimeout,
                StaleTimeout = options?.StaleTimeout ?? _config.StaleTimeout,
                ExecutionTimeout = executionTimeout,
                CancelRetryInterval = options?.CancelRetryInterval ?? _config.CancelRetryInterval,
                TombstoneTtl = options?.TombstoneTtl ?? _config.TombstoneTtl,
            };
        }

        // --- internal inspection hooks for the bounded-state tests ---
        internal int InflightCount
        {
            get { lock (_runtime.Gate) { return _inflight.Count; } }
        }

        internal int TombstoneCount
        {
            get { lock (_runtime.Gate) { return _tombstones.Count; } }
        }

        internal int LiveTimerCount
        {
            get { lock (_runtime.Gate) { return _runtime.LiveTimerCount; } }
        }

        private enum CommandState
        {
            Sending,
            Live,
        }

        private sealed class CommandOptions
        {
            public TimeSpan RequestRetryInterval { get; init; }

            public TimeSpan NoAckTimeout { get; init; }

            public TimeSpan StaleTimeout { get; init; }

            public TimeSpan? ExecutionTimeout { get; init; }

            public TimeSpan CancelRetryInterval { get; init; }

            public TimeSpan TombstoneTtl { get; init; }
        }

        private sealed class Command
        {
            public Command(
                string id,
                TaskCompletionSource<ParleyResult<TResult, TError>> completionSource,
                IProgress<TProgress>? progress,
                CommandOptions options)
            {
                Id = id;
                CompletionSource = completionSource;
                Progress = progress;
                Options = options;
            }

            public string Id { get; }

            public TaskCompletionSource<ParleyResult<TResult, TError>> CompletionSource { get; }

            public IProgress<TProgress>? Progress { get; }

            public CommandOptions Options { get; }

            public CommandState State { get; set; } = CommandState.Sending;

            public bool Cancelling { get; set; }

            public byte[]? RequestBytes { get; set; }

            public byte[]? CancelBytes { get; set; }

            public ParleyRuntime.ParleyTimer? RequestRetryTimer { get; set; }

            public ParleyRuntime.ParleyTimer? SignOfLifeTimer { get; set; }

            public ParleyRuntime.ParleyTimer? ExecutionCapTimer { get; set; }

            public ParleyRuntime.ParleyTimer? CancelRetryTimer { get; set; }

            public CancellationTokenRegistration CancellationRegistration { get; set; }
        }
    }
}
