using System;

namespace Parley
{
    /// <summary>
    /// Per-call overrides of the commander-local timing parameters (protocol.md §8).
    /// Any property left <see langword="null"/> falls back to the <see cref="ParleyConfig"/> value.
    /// The progress sink and cancellation are passed as separate arguments to
    /// <see cref="ParleyCommander{TRequest,TProgress,TResult,TError}.ExecuteAsync"/>.
    /// </summary>
    public sealed class ParleyCallOptions
    {
        /// <summary>Overrides <see cref="ParleyConfig.RequestRetryInterval"/> for this call.</summary>
        public TimeSpan? RequestRetryInterval { get; init; }

        /// <summary>Overrides <see cref="ParleyConfig.NoAckTimeout"/> for this call.</summary>
        public TimeSpan? NoAckTimeout { get; init; }

        /// <summary>Overrides <see cref="ParleyConfig.StaleTimeout"/> for this call.</summary>
        public TimeSpan? StaleTimeout { get; init; }

        /// <summary>Overrides <see cref="ParleyConfig.ExecutionTimeout"/> for this call.
        /// When present, <see cref="ExecutionTimeout"/> supplies the value and
        /// <see cref="DisableExecutionTimeout"/> can turn the cap off entirely.</summary>
        public TimeSpan? ExecutionTimeout { get; init; }

        /// <summary>When <see langword="true"/>, disables the execution cap for this call regardless of
        /// <see cref="ExecutionTimeout"/> or the configured default.</summary>
        public bool DisableExecutionTimeout { get; init; }

        /// <summary>Overrides <see cref="ParleyConfig.CancelRetryInterval"/> for this call.</summary>
        public TimeSpan? CancelRetryInterval { get; init; }

        /// <summary>Overrides <see cref="ParleyConfig.TombstoneTtl"/> for this call.</summary>
        public TimeSpan? TombstoneTtl { get; init; }
    }
}
