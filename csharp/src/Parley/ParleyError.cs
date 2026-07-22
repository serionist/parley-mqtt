namespace Parley
{
    /// <summary>
    /// The error arm of a <see cref="ParleyResult{TResult,TError}"/>: a builtin protocol code
    /// (<see cref="IsBuiltin"/>) or a consumer-defined custom code, plus optional human-readable detail.
    /// </summary>
    /// <typeparam name="TError">The consumer's custom error-code type.</typeparam>
    public readonly struct ParleyError<TError>
    {
        private readonly ParleyBuiltinError _builtin;
        private readonly bool _isBuiltin;
        private readonly TError? _custom;

        internal ParleyError(string code, ParleyBuiltinError builtin, TError? custom, string? message, bool isBuiltin)
        {
            Code = code;
            _builtin = builtin;
            _custom = custom;
            Message = message;
            _isBuiltin = isBuiltin;
        }

        /// <summary>The raw wire error code string (e.g. <c>"$busy"</c> or a custom code).</summary>
        public string Code { get; }

        /// <summary>Optional human-readable detail (the <c>m</c> field), when supplied by the handler.</summary>
        public string? Message { get; }

        /// <summary><see langword="true"/> when <see cref="Code"/> is one of the six builtin protocol codes.</summary>
        public bool IsBuiltin => _isBuiltin;

        /// <summary>The builtin code. Only meaningful when <see cref="IsBuiltin"/> is <see langword="true"/>.</summary>
        public ParleyBuiltinError Builtin => _builtin;

        /// <summary>The typed custom error. Only meaningful when <see cref="IsBuiltin"/> is <see langword="false"/>.</summary>
        public TError? Custom => _custom;

        /// <summary>Returns the wire code string.</summary>
        /// <returns><see cref="Code"/>.</returns>
        public override string ToString() => Code;
    }
}
