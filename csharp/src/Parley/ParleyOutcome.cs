namespace Parley
{
    internal enum ParleyOutcomeKind
    {
        Success,
        Error,
        Cancelled,
    }

    /// <summary>
    /// The outcome a <see cref="ParleyCommandHandler{TRequest,TProgress,TResult,TError}"/> returns for one
    /// command: success with a result, a custom error, or cancellation. Construct instances via the
    /// <see cref="ParleyOutcome"/> factory methods.
    /// </summary>
    /// <typeparam name="TResult">The result payload type.</typeparam>
    /// <typeparam name="TError">The consumer's custom error-code type.</typeparam>
    public readonly struct ParleyOutcome<TResult, TError>
    {
        internal ParleyOutcome(ParleyOutcomeKind kind, TResult? value, TError? error, string? message)
        {
            Kind = kind;
            Value = value;
            Error = error;
            Message = message;
        }

        internal ParleyOutcomeKind Kind { get; }

        internal TResult? Value { get; }

        internal TError? Error { get; }

        internal string? Message { get; }

        /// <summary>Implicitly lifts a <see cref="ParleyOutcome.Success{TResult}(TResult)"/> token.</summary>
        /// <param name="token">The success token.</param>
        public static implicit operator ParleyOutcome<TResult, TError>(ParleySuccessOutcome<TResult> token) =>
            new(ParleyOutcomeKind.Success, token.Value, default, null);

        /// <summary>Implicitly lifts a <see cref="ParleyOutcome.Error{TError}(TError, string)"/> token.</summary>
        /// <param name="token">The error token.</param>
        public static implicit operator ParleyOutcome<TResult, TError>(ParleyErrorOutcome<TError> token) =>
            new(ParleyOutcomeKind.Error, default, token.Error, token.Message);

        /// <summary>Implicitly lifts a <see cref="ParleyOutcome.Cancelled"/> token.</summary>
        /// <param name="token">The cancelled token.</param>
        public static implicit operator ParleyOutcome<TResult, TError>(ParleyCancelledOutcome token) =>
            new(ParleyOutcomeKind.Cancelled, default, default, null);
    }

    /// <summary>An intermediate success token; convert to a <see cref="ParleyOutcome{TResult,TError}"/> by returning it.</summary>
    /// <typeparam name="TResult">The result payload type.</typeparam>
    public readonly struct ParleySuccessOutcome<TResult>
    {
        internal ParleySuccessOutcome(TResult value) => Value = value;

        internal TResult Value { get; }
    }

    /// <summary>An intermediate custom-error token; convert to a <see cref="ParleyOutcome{TResult,TError}"/> by returning it.</summary>
    /// <typeparam name="TError">The consumer's custom error-code type.</typeparam>
    public readonly struct ParleyErrorOutcome<TError>
    {
        internal ParleyErrorOutcome(TError error, string? message)
        {
            Error = error;
            Message = message;
        }

        internal TError Error { get; }

        internal string? Message { get; }
    }

    /// <summary>An intermediate cancelled token; convert to any <see cref="ParleyOutcome{TResult,TError}"/> by returning it.</summary>
    public readonly struct ParleyCancelledOutcome
    {
    }

    /// <summary>Factory methods for <see cref="ParleyOutcome{TResult,TError}"/> values.</summary>
    public static class ParleyOutcome
    {
        /// <summary>A successful outcome carrying a result payload.</summary>
        /// <typeparam name="TResult">The result payload type.</typeparam>
        /// <param name="value">The result payload.</param>
        /// <returns>A token that converts to a <see cref="ParleyOutcome{TResult,TError}"/>.</returns>
        public static ParleySuccessOutcome<TResult> Success<TResult>(TResult value) => new(value);

        /// <summary>A custom-error outcome. The error must not map to a <c>$</c>-prefixed wire code.</summary>
        /// <typeparam name="TError">The consumer's custom error-code type.</typeparam>
        /// <param name="error">The custom error value.</param>
        /// <param name="message">Optional human-readable detail (the <c>m</c> field).</param>
        /// <returns>A token that converts to a <see cref="ParleyOutcome{TResult,TError}"/>.</returns>
        public static ParleyErrorOutcome<TError> Error<TError>(TError error, string? message = null) => new(error, message);

        /// <summary>A cancelled outcome, emitted on the wire as <c>$cancelled</c>.</summary>
        /// <returns>A token that converts to any <see cref="ParleyOutcome{TResult,TError}"/>.</returns>
        public static ParleyCancelledOutcome Cancelled() => default;
    }
}
