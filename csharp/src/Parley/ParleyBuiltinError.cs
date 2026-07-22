namespace Parley
{
    /// <summary>
    /// The five builtin error codes reserved by the protocol (protocol.md §5). Their wire form is the
    /// matching <c>$</c>-prefixed string; consumer (custom) codes must never begin with <c>$</c>.
    /// </summary>
    public enum ParleyBuiltinError
    {
        /// <summary><c>$noAck</c> — the sign-of-life window expired before any sign of life (handler unreachable).</summary>
        NoAck,

        /// <summary><c>$executionStale</c> — the sign-of-life window expired after signs of life (handler went silent).</summary>
        ExecutionStale,

        /// <summary><c>$executionTimeout</c> — the overall execution cap expired (handler alive but never finished).</summary>
        ExecutionTimeout,

        /// <summary><c>$cancelled</c> — the command was cancelled before producing a result.</summary>
        Cancelled,

        /// <summary><c>$busy</c> — the handler refused admission because its command pool is full.</summary>
        Busy,
    }
}
