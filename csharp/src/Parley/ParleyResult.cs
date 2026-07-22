using System;

namespace Parley
{
    /// <summary>
    /// The single terminal outcome of a command observed by the commander: success with a typed result,
    /// or an error (builtin or custom). Every expected outcome — including all six builtin codes — is
    /// returned as a value; exceptions are reserved for programming errors.
    /// </summary>
    /// <typeparam name="TResult">The result payload type.</typeparam>
    /// <typeparam name="TError">The consumer's custom error-code type.</typeparam>
    public readonly struct ParleyResult<TResult, TError>
    {
        private readonly TResult? _value;
        private readonly ParleyError<TError> _error;

        private ParleyResult(bool isSuccess, TResult? value, ParleyError<TError> error)
        {
            IsSuccess = isSuccess;
            _value = value;
            _error = error;
        }

        /// <summary><see langword="true"/> when the command succeeded.</summary>
        public bool IsSuccess { get; }

        /// <summary><see langword="true"/> when the command produced an error.</summary>
        public bool IsError => !IsSuccess;

        /// <summary>The result payload. Only meaningful when <see cref="IsSuccess"/> is <see langword="true"/>.</summary>
        public TResult? Value => _value;

        /// <summary>The error. Only meaningful when <see cref="IsSuccess"/> is <see langword="false"/>.</summary>
        public ParleyError<TError> Error =>
            IsSuccess ? throw new InvalidOperationException("Result is a success; there is no error.") : _error;

        internal static ParleyResult<TResult, TError> Success(TResult? value) =>
            new(true, value, default);

        internal static ParleyResult<TResult, TError> Failure(ParleyError<TError> error) =>
            new(false, default, error);
    }
}
