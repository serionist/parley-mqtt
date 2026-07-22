using System;

namespace Parley
{
    /// <summary>
    /// The wire strings for the builtin error codes and helpers for mapping between the strings
    /// and <see cref="ParleyBuiltinError"/>.
    /// </summary>
    public static class ParleyErrorCodes
    {
        /// <summary><c>$noAck</c>.</summary>
        public const string NoAck = "$noAck";

        /// <summary><c>$executionStale</c>.</summary>
        public const string ExecutionStale = "$executionStale";

        /// <summary><c>$executionTimeout</c>.</summary>
        public const string ExecutionTimeout = "$executionTimeout";

        /// <summary><c>$cancelled</c>.</summary>
        public const string Cancelled = "$cancelled";

        /// <summary><c>$busy</c>.</summary>
        public const string Busy = "$busy";

        /// <summary><c>$decodeError</c> — commander-local only; never sent on the wire.</summary>
        public const string DecodeError = "$decodeError";

        /// <summary>Returns the wire string for a builtin code.</summary>
        /// <param name="builtin">The builtin code.</param>
        /// <returns>The <c>$</c>-prefixed wire string.</returns>
        public static string ToWire(ParleyBuiltinError builtin) => builtin switch
        {
            ParleyBuiltinError.NoAck => NoAck,
            ParleyBuiltinError.ExecutionStale => ExecutionStale,
            ParleyBuiltinError.ExecutionTimeout => ExecutionTimeout,
            ParleyBuiltinError.Cancelled => Cancelled,
            ParleyBuiltinError.Busy => Busy,
            ParleyBuiltinError.DecodeError => DecodeError,
            _ => throw new ArgumentOutOfRangeException(nameof(builtin), builtin, null),
        };

        /// <summary>Attempts to map a wire error string onto a builtin code.</summary>
        /// <param name="code">The wire error string.</param>
        /// <param name="builtin">The mapped builtin code, when the return value is <see langword="true"/>.</param>
        /// <returns><see langword="true"/> when <paramref name="code"/> is one of the five builtin codes.</returns>
        public static bool TryParse(string? code, out ParleyBuiltinError builtin)
        {
            switch (code)
            {
                case NoAck:
                    builtin = ParleyBuiltinError.NoAck;
                    return true;
                case ExecutionStale:
                    builtin = ParleyBuiltinError.ExecutionStale;
                    return true;
                case ExecutionTimeout:
                    builtin = ParleyBuiltinError.ExecutionTimeout;
                    return true;
                case Cancelled:
                    builtin = ParleyBuiltinError.Cancelled;
                    return true;
                case Busy:
                    builtin = ParleyBuiltinError.Busy;
                    return true;
                case DecodeError:
                    builtin = ParleyBuiltinError.DecodeError;
                    return true;
                default:
                    builtin = default;
                    return false;
            }
        }
    }
}
