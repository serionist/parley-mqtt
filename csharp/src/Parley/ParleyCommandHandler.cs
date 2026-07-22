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
    /// The callee side of one command type (protocol.md §7, rules H1–H8). The consumer overrides
    /// <see cref="HandleCommandAsync"/>. A topic set MUST have at most one active handler. Admission
    /// (dedup lookup, capacity check, slot reservation, and the first heartbeat) is one atomic step.
    /// </summary>
    /// <typeparam name="TRequest">The request payload type.</typeparam>
    /// <typeparam name="TProgress">The progress payload type.</typeparam>
    /// <typeparam name="TResult">The result payload type.</typeparam>
    /// <typeparam name="TError">The consumer's custom error-code type.</typeparam>
    public abstract class ParleyCommandHandler<TRequest, TProgress, TResult, TError> : IDisposable
    {
        private readonly ParleyConnectionAdapter _adapter;
        private readonly ParleyTopics _topics;
        private readonly ParleyConfig _config;
        private readonly ParleyRuntime _runtime;
        private readonly JsonTypeInfo<TRequest> _requestTypeInfo;
        private readonly JsonTypeInfo<TProgress> _progressTypeInfo;
        private readonly JsonTypeInfo<TResult> _resultTypeInfo;
        private readonly JsonTypeInfo<TError> _errorTypeInfo;
        private readonly Dictionary<string, HandlerCommand> _commands = new();
        private readonly LinkedList<HandlerCommand> _queue = new();
        private readonly TombstoneTable _tombstones;
        private int _executingCount;
        private bool _dispatching;
        private bool _disposed;

        /// <summary>Creates a handler for one command type.</summary>
        /// <param name="adapter">The transport adapter.</param>
        /// <param name="topics">The four topic strings for this command type.</param>
        /// <param name="config">Timing configuration and capacity caps.</param>
        /// <param name="jsonContext">A source-generated context providing type info for the four payload types.</param>
        /// <param name="timeProvider">Clock; defaults to <see cref="TimeProvider.System"/>.</param>
        /// <exception cref="ArgumentNullException">A required argument is null.</exception>
        /// <exception cref="ArgumentException">The config is invalid or a payload type has no source-gen metadata.</exception>
        protected ParleyCommandHandler(
            ParleyConnectionAdapter adapter,
            ParleyTopics topics,
            ParleyConfig config,
            JsonSerializerContext jsonContext,
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
            _runtime = new ParleyRuntime(timeProvider ?? TimeProvider.System);
            _tombstones = new TombstoneTable(_runtime, _config.MaxTombstones);

            _requestTypeInfo = TypeInfoResolver.Resolve<TRequest>(jsonContext);
            _progressTypeInfo = TypeInfoResolver.Resolve<TProgress>(jsonContext);
            _resultTypeInfo = TypeInfoResolver.Resolve<TResult>(jsonContext);
            _errorTypeInfo = TypeInfoResolver.Resolve<TError>(jsonContext);

            _adapter.Connected += OnConnected;
            SubscribeAll();
        }

        /// <summary>
        /// Executes one admitted command. Return a <see cref="ParleyOutcome{TResult,TError}"/> value; expected
        /// failures are values, not exceptions. Progress is emitted through <paramref name="execution"/>, and
        /// <paramref name="cancellationToken"/> (equal to <see cref="IParleyExecution{TProgress}.CancellationToken"/>)
        /// is signalled when a <c>cnl</c> arrives (protocol.md H7).
        /// </summary>
        /// <param name="request">The request payload.</param>
        /// <param name="execution">The execution context (progress emitter, cancellation, command id).</param>
        /// <param name="cancellationToken">The cancellation signal for this command.</param>
        /// <returns>The command outcome.</returns>
        protected abstract Task<ParleyOutcome<TResult, TError>> HandleCommandAsync(
            TRequest request,
            IParleyExecution<TProgress> execution,
            CancellationToken cancellationToken);

        private void SubscribeAll() => _adapter.Subscribe(_topics.Request, (_, payload) => OnRequestTopic(payload));

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

        private void OnRequestTopic(ReadOnlyMemory<byte> payload)
        {
            Envelope? env = MessageCodec.TryParse(payload);
            if (env is null || env.V != MessageCodec.Version || !MessageCodec.IsValidId(env.Id))
            {
                return;
            }

            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    return;
                }

                switch (env.T)
                {
                    case MessageTypes.Request:
                        OnRequest(env.Id!, env.P);
                        break;
                    case MessageTypes.ResultAck:
                        OnResultAck(env.Id!);
                        break;
                    case MessageTypes.Cancel:
                        OnCancel(env.Id!);
                        break;
                    default:
                        // Unknown type, or a handler→commander type on the request topic — ignore (§4).
                        break;
                }
            }
        }

        private void OnRequest(string id, JsonElement? payload)
        {
            // H1: dedup FIRST — the capacity check applies only to unknown ids.
            if (_commands.TryGetValue(id, out HandlerCommand? existing))
            {
                switch (existing.State)
                {
                    case HandlerState.Queued:
                    case HandlerState.Executing:
                        PublishHeartbeat(existing); // immediate hb resynchronizes the commander cheaply
                        break;
                    case HandlerState.ResultCached:
                        RepublishCachedResult(existing);
                        break;
                }

                return;
            }

            // A duplicate of a tombstoned id is ignored (H1).
            if (_tombstones.Contains(id))
            {
                return;
            }

            // H2/H8: unknown id — admit if a command slot is free, otherwise stateless $busy.
            if (_commands.Count >= _config.MaxActiveCommands)
            {
                PublishStatelessError(id, ParleyErrorCodes.Busy);
                return;
            }

            TRequest request;
            try
            {
                request = payload.HasValue
                    ? MessageCodec.FromElement(payload.Value, _requestTypeInfo)!
                    : default!;
            }
            catch (Exception ex) when (ex is JsonException or NotSupportedException)
            {
                // §4 robustness: a req whose payload does not decode to TRequest is silently ignored — no
                // admission, no state, no reply.
                return;
            }

            var cmd = new HandlerCommand(id, request);
            cmd.Execution = new Execution(this, id, cmd.Cancellation.Token);
            cmd.QueueNode = _queue.AddLast(cmd);
            _commands[id] = cmd;

            // H2: the first heartbeat is the accept-ack, published immediately.
            PublishHeartbeat(cmd);
            TryDispatch();
        }

        private void OnResultAck(string id)
        {
            // H6: a rak for an id not in RESULT-CACHED is ignored.
            if (_commands.TryGetValue(id, out HandlerCommand? cmd) && cmd.State == HandlerState.ResultCached)
            {
                MoveToTombstone(cmd);
            }
        }

        private void OnCancel(string id)
        {
            if (_commands.TryGetValue(id, out HandlerCommand? cmd))
            {
                switch (cmd.State)
                {
                    case HandlerState.Executing:
                        // H7: trigger the cancellation signal; the eventual outcome is what the handler produces.
                        cmd.Cancellation.Cancel();
                        break;
                    case HandlerState.Queued:
                        // H7: dequeue and produce a cached, retried $cancelled result.
                        _queue.Remove(cmd.QueueNode!);
                        cmd.QueueNode = null;
                        CompleteWithResult(cmd, BuildBuiltinErrorBytes(id, ParleyErrorCodes.Cancelled), wasExecuting: false);
                        break;
                    case HandlerState.ResultCached:
                        // H7: cancel lost the race — re-publish the cached result.
                        RepublishCachedResult(cmd);
                        break;
                }

                return;
            }

            // H7: unknown or tombstoned id — stateless $cancelled (no state, no rak expected).
            PublishStatelessError(id, ParleyErrorCodes.Cancelled);
        }

        private void TryDispatch()
        {
            if (_dispatching)
            {
                return;
            }

            _dispatching = true;
            try
            {
                // H4: dequeue FIFO into at most maxConcurrentExecutions slots.
                while (_executingCount < _config.MaxConcurrentExecutions && _queue.First is { } node)
                {
                    HandlerCommand cmd = node.Value;
                    _queue.RemoveFirst();
                    cmd.QueueNode = null;
                    cmd.State = HandlerState.Executing;
                    _executingCount++;
                    _ = RunAsync(cmd);
                }
            }
            finally
            {
                _dispatching = false;
            }
        }

        private async Task RunAsync(HandlerCommand cmd)
        {
            ParleyOutcome<TResult, TError> outcome;
            try
            {
                outcome = await HandleCommandAsync(cmd.Request, cmd.Execution!, cmd.Cancellation.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                outcome = ParleyOutcome.Cancelled();
            }
            catch (Exception)
            {
                // A programming error escaped the consumer handler: abort the command so the slot is freed
                // and state stays bounded. No result is produced; the commander ends via its own timers.
                lock (_runtime.Gate)
                {
                    if (!_disposed)
                    {
                        AbortExecuting(cmd);
                    }
                }

                return;
            }

            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    return;
                }

                // CR3/AB1: build the outcome bytes BEFORE mutating any state. Serialization can throw (a TError
                // that is not a plain JSON string / is $-reserved, or an unserializable TResult); the settled
                // contract treats that exactly like a thrown handler — abort so the slot is freed, timers
                // disposed, and the id tombstoned, rather than leaking the slot and heartbeat forever.
                byte[] resultBytes;
                try
                {
                    resultBytes = BuildOutcomeBytes(cmd.Id, outcome);
                }
                catch (Exception)
                {
                    AbortExecuting(cmd);
                    return;
                }

                CompleteWithResult(cmd, resultBytes, wasExecuting: true);
            }
        }

        private void CompleteWithResult(HandlerCommand cmd, byte[] resultBytes, bool wasExecuting)
        {
            // H3: heartbeats run until the command's first res is published.
            cmd.HeartbeatTimer?.Dispose();
            cmd.HeartbeatTimer = null;

            cmd.CachedResultBytes = resultBytes;
            cmd.State = HandlerState.ResultCached;
            Publish(_topics.Result, resultBytes);

            // H5: retry the result until rak or resultRetryTimeout (measured from this first publication).
            cmd.ResultRetryTimer = _runtime.Schedule(_config.ResultRetryInterval, () => OnResultRetry(cmd));
            cmd.ResultRetryTimeoutTimer = _runtime.Schedule(_config.ResultRetryTimeout, () => MoveToTombstone(cmd));

            if (wasExecuting)
            {
                _executingCount--;
                TryDispatch();
            }
        }

        private void AbortExecuting(HandlerCommand cmd)
        {
            cmd.HeartbeatTimer?.Dispose();
            cmd.HeartbeatTimer = null;
            _executingCount--;
            _commands.Remove(cmd.Id);
            cmd.Cancellation.Dispose();
            _tombstones.Add(cmd.Id, _config.TombstoneTtl);
            TryDispatch();
        }

        private void OnResultRetry(HandlerCommand cmd)
        {
            // Reschedule before publishing so a throwing adapter (CR4) never kills the retry cadence — the
            // re-send self-heals once the adapter recovers.
            cmd.ResultRetryTimer!.Reschedule(_config.ResultRetryInterval);
            Publish(_topics.Result, cmd.CachedResultBytes!);
        }

        private void MoveToTombstone(HandlerCommand cmd)
        {
            cmd.ResultRetryTimer?.Dispose();
            cmd.ResultRetryTimeoutTimer?.Dispose();
            cmd.HeartbeatTimer?.Dispose();
            cmd.ResultRetryTimer = null;
            cmd.ResultRetryTimeoutTimer = null;
            cmd.HeartbeatTimer = null;
            cmd.Cancellation.Dispose();

            // H8: the admission slot is freed at the tombstone transition.
            _commands.Remove(cmd.Id);
            _tombstones.Add(cmd.Id, _config.TombstoneTtl);
        }

        private void PublishHeartbeat(HandlerCommand cmd)
        {
            // AB3: the hb envelope is constant per id — serialize it once and reuse the cached bytes.
            cmd.CachedHeartbeatBytes ??= MessageCodec.Serialize(
                new Envelope { V = MessageCodec.Version, T = MessageTypes.Heartbeat, Id = cmd.Id });

            // H3: one cadence per id, restarted by every hb publication. (Re)schedule BEFORE publishing so a
            // throwing adapter (CR4) never kills the cadence.
            if (cmd.HeartbeatTimer is null)
            {
                cmd.HeartbeatTimer = _runtime.Schedule(_config.HeartbeatInterval, () => PublishHeartbeat(cmd));
            }
            else
            {
                cmd.HeartbeatTimer.Reschedule(_config.HeartbeatInterval);
            }

            Publish(_topics.Heartbeat, cmd.CachedHeartbeatBytes);
        }

        private void RepublishCachedResult(HandlerCommand cmd) => Publish(_topics.Result, cmd.CachedResultBytes!);

        private void PublishStatelessError(string id, string code) =>
            Publish(_topics.Result, BuildBuiltinErrorBytes(id, code));

        private void EmitProgress(string id, TProgress progress)
        {
            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    return;
                }

                JsonElement? payload = MessageCodec.ToPayloadElement(progress, _progressTypeInfo);
                byte[] bytes = MessageCodec.Serialize(new Envelope { V = MessageCodec.Version, T = MessageTypes.Progress, Id = id, P = payload });
                Publish(_topics.Progress, bytes);
            }
        }

        private byte[] BuildOutcomeBytes(string id, ParleyOutcome<TResult, TError> outcome)
        {
            switch (outcome.Kind)
            {
                case ParleyOutcomeKind.Success:
                    return MessageCodec.Serialize(new Envelope
                    {
                        V = MessageCodec.Version,
                        T = MessageTypes.Result,
                        Id = id,
                        Ok = true,
                        P = MessageCodec.ToPayloadElement(outcome.Value!, _resultTypeInfo),
                    });

                case ParleyOutcomeKind.Error:
                    return MessageCodec.Serialize(new Envelope
                    {
                        V = MessageCodec.Version,
                        T = MessageTypes.Result,
                        Id = id,
                        Ok = false,
                        E = ErrorCodec.CustomToWire(outcome.Error!, _errorTypeInfo),
                        M = outcome.Message,
                    });

                case ParleyOutcomeKind.Cancelled:
                default:
                    return BuildBuiltinErrorBytes(id, ParleyErrorCodes.Cancelled);
            }
        }

        private static byte[] BuildBuiltinErrorBytes(string id, string code) =>
            MessageCodec.Serialize(new Envelope
            {
                V = MessageCodec.Version,
                T = MessageTypes.Result,
                Id = id,
                Ok = false,
                E = code,
            });

        private void Publish(string topic, byte[] bytes) => _adapter.Publish(topic, bytes);

        /// <summary>
        /// Disposes the handler (design.md): detaches the connected callback, unsubscribes the request topic,
        /// disposes every per-command timer, signals and releases every command's cancellation source, and clears
        /// all state. Idempotent. After disposal, inbound transport callbacks are ignored.
        /// </summary>
        public void Dispose()
        {
            var sources = new List<CancellationTokenSource>();

            lock (_runtime.Gate)
            {
                if (_disposed)
                {
                    return;
                }

                _disposed = true;

                _adapter.Connected -= OnConnected;
                _adapter.Unsubscribe(_topics.Request);

                foreach (HandlerCommand cmd in _commands.Values)
                {
                    cmd.HeartbeatTimer?.Dispose();
                    cmd.ResultRetryTimer?.Dispose();
                    cmd.ResultRetryTimeoutTimer?.Dispose();
                    cmd.HeartbeatTimer = null;
                    cmd.ResultRetryTimer = null;
                    cmd.ResultRetryTimeoutTimer = null;
                    sources.Add(cmd.Cancellation);
                }

                _commands.Clear();
                _queue.Clear();
                _executingCount = 0;
                _tombstones.Clear();
            }

            // Signal any executing handler to stop and release the sources OUTSIDE the gate, so consumer
            // cancellation callbacks never run under it (mirror of the commander's registration handling).
            foreach (CancellationTokenSource source in sources)
            {
                try
                {
                    source.Cancel();
                }
                catch
                {
                    // A consumer cancellation callback threw; disposal must still complete for the rest.
                }

                source.Dispose();
            }
        }

        // --- internal inspection hooks for the bounded-state tests ---
        internal int CommandCount
        {
            get { lock (_runtime.Gate) { return _commands.Count; } }
        }

        internal int QueueCount
        {
            get { lock (_runtime.Gate) { return _queue.Count; } }
        }

        internal int ExecutingCount
        {
            get { lock (_runtime.Gate) { return _executingCount; } }
        }

        internal int TombstoneCount
        {
            get { lock (_runtime.Gate) { return _tombstones.Count; } }
        }

        internal int LiveTimerCount
        {
            get { lock (_runtime.Gate) { return _runtime.LiveTimerCount; } }
        }

        private enum HandlerState
        {
            Queued,
            Executing,
            ResultCached,
        }

        private sealed class Execution : IParleyExecution<TProgress>
        {
            private readonly ParleyCommandHandler<TRequest, TProgress, TResult, TError> _handler;

            public Execution(ParleyCommandHandler<TRequest, TProgress, TResult, TError> handler, string commandId, CancellationToken cancellationToken)
            {
                _handler = handler;
                CommandId = commandId;
                CancellationToken = cancellationToken;
            }

            public string CommandId { get; }

            public CancellationToken CancellationToken { get; }

            public void ReportProgress(TProgress progress) => _handler.EmitProgress(CommandId, progress);
        }

        private sealed class HandlerCommand
        {
            public HandlerCommand(string id, TRequest request)
            {
                Id = id;
                Request = request;
            }

            public string Id { get; }

            public TRequest Request { get; }

            public HandlerState State { get; set; } = HandlerState.Queued;

            public CancellationTokenSource Cancellation { get; } = new CancellationTokenSource();

            public Execution? Execution { get; set; }

            public LinkedListNode<HandlerCommand>? QueueNode { get; set; }

            public byte[]? CachedHeartbeatBytes { get; set; }

            public byte[]? CachedResultBytes { get; set; }

            public ParleyRuntime.ParleyTimer? HeartbeatTimer { get; set; }

            public ParleyRuntime.ParleyTimer? ResultRetryTimer { get; set; }

            public ParleyRuntime.ParleyTimer? ResultRetryTimeoutTimer { get; set; }
        }
    }
}
