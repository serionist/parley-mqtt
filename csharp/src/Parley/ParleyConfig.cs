using System;

namespace Parley
{
    /// <summary>
    /// Per-deployment timing configuration and handler capacity caps (protocol.md §8).
    /// Values are not negotiated over the wire; both peers are configured independently.
    /// All properties are optional and default to the protocol.md §8 defaults.
    /// </summary>
    public sealed class ParleyConfig
    {
        /// <summary>C1 — interval between <c>req</c> re-publishes while SENDING. Default 2 s.</summary>
        public TimeSpan RequestRetryInterval { get; init; } = TimeSpan.FromSeconds(2);

        /// <summary>C3 — sign-of-life window before any sign of life (SENDING). Default 10 s.</summary>
        public TimeSpan NoAckTimeout { get; init; } = TimeSpan.FromSeconds(10);

        /// <summary>C3 — sliding sign-of-life window after signs of life begin (LIVE). Default 15 s.</summary>
        public TimeSpan StaleTimeout { get; init; } = TimeSpan.FromSeconds(15);

        /// <summary>C2/C3 — overall execution cap measured from the first sign of life. Default 60 s.
        /// Set to <see langword="null"/> to disable the cap.</summary>
        public TimeSpan? ExecutionTimeout { get; init; } = TimeSpan.FromSeconds(60);

        /// <summary>C7 — interval between <c>cnl</c> re-publishes while cancelling. Default 2 s.</summary>
        public TimeSpan CancelRetryInterval { get; init; } = TimeSpan.FromSeconds(2);

        /// <summary>H3 — interval between <c>hb</c> publications per admitted command. Default 3 s.</summary>
        public TimeSpan HeartbeatInterval { get; init; } = TimeSpan.FromSeconds(3);

        /// <summary>H5 — interval between <c>res</c> re-publishes until acked. Default 2 s.</summary>
        public TimeSpan ResultRetryInterval { get; init; } = TimeSpan.FromSeconds(2);

        /// <summary>H5 — give-up window for result retries, measured from the first <c>res</c> publication. Default 30 s.</summary>
        public TimeSpan ResultRetryTimeout { get; init; } = TimeSpan.FromSeconds(30);

        /// <summary>C5/H5 — how long a terminal id is retained as a tombstone for duplicate suppression. Default 60 s.</summary>
        public TimeSpan TombstoneTtl { get; init; } = TimeSpan.FromSeconds(60);

        /// <summary>H4 — maximum number of commands executing concurrently. Default 1 (serial).</summary>
        public int MaxConcurrentExecutions { get; init; } = 1;

        /// <summary>H2/H8 — admission cap; a slot is held from admission through RESULT-CACHED. Default 32.</summary>
        public int MaxActiveCommands { get; init; } = 32;

        /// <summary>Hard cap on the tombstone table size; oldest entries are evicted first when exceeded
        /// (C5 / H8). Bounds state independently of the TTL. Default 4096.</summary>
        public int MaxTombstones { get; init; } = 4096;

        internal void Validate()
        {
            RequirePositive(RequestRetryInterval, nameof(RequestRetryInterval));
            RequirePositive(NoAckTimeout, nameof(NoAckTimeout));
            RequirePositive(StaleTimeout, nameof(StaleTimeout));
            if (ExecutionTimeout is { } cap)
            {
                RequirePositive(cap, nameof(ExecutionTimeout));
            }

            RequirePositive(CancelRetryInterval, nameof(CancelRetryInterval));
            RequirePositive(HeartbeatInterval, nameof(HeartbeatInterval));
            RequirePositive(ResultRetryInterval, nameof(ResultRetryInterval));
            RequirePositive(ResultRetryTimeout, nameof(ResultRetryTimeout));
            RequirePositive(TombstoneTtl, nameof(TombstoneTtl));

            if (MaxConcurrentExecutions < 1)
            {
                throw new ArgumentException("MaxConcurrentExecutions must be at least 1.", nameof(MaxConcurrentExecutions));
            }

            if (MaxActiveCommands < 1)
            {
                throw new ArgumentException("MaxActiveCommands must be at least 1.", nameof(MaxActiveCommands));
            }

            if (MaxTombstones < 1)
            {
                throw new ArgumentException("MaxTombstones must be at least 1.", nameof(MaxTombstones));
            }
        }

        private static void RequirePositive(TimeSpan value, string name)
        {
            if (value <= TimeSpan.Zero)
            {
                throw new ArgumentException("Duration must be strictly positive.", name);
            }
        }
    }
}
