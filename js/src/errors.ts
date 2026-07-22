/**
 * The six builtin protocol error codes (protocol.md §5). Their wire form is the matching
 * `$`-prefixed string; the value equals the code, so a builtin `ParleyError.code === error.builtin`.
 */
export const ParleyBuiltinError = {
  /** `$noAck` — the sign-of-life window expired before any sign of life (handler unreachable). */
  NoAck: '$noAck',
  /** `$executionStale` — the sign-of-life window expired after signs of life (handler went silent). */
  ExecutionStale: '$executionStale',
  /** `$executionTimeout` — the overall execution cap expired (handler alive but never finished). */
  ExecutionTimeout: '$executionTimeout',
  /** `$cancelled` — the command was cancelled before producing a result. */
  Cancelled: '$cancelled',
  /** `$busy` — the handler refused admission because its command pool is full. */
  Busy: '$busy',
  /** `$decodeError` — a success `res` arrived but its payload did not decode to the expected type
   *  (commander-local; never sent on the wire). */
  DecodeError: '$decodeError',
} as const;

/** One of the six builtin error code strings. */
export type ParleyBuiltinErrorCode = (typeof ParleyBuiltinError)[keyof typeof ParleyBuiltinError];

/** Alias kept for symmetry with the other language bindings' `ParleyErrorCodes`. */
export const ParleyErrorCodes = ParleyBuiltinError;

const BUILTIN_SET = new Set<string>(Object.values(ParleyBuiltinError));

/** Maps a wire error string onto a builtin code, or `undefined` when it is a custom code. */
export function toBuiltin(code: string): ParleyBuiltinErrorCode | undefined {
  return BUILTIN_SET.has(code) ? (code as ParleyBuiltinErrorCode) : undefined;
}

/**
 * The error arm of a {@link ParleyResult}: a builtin protocol code (`builtin` set) or a consumer
 * custom code (`custom` set), plus the raw wire `code` and optional human-readable `message` (`m`).
 */
export interface ParleyError<TError> {
  /** The raw wire error code string (e.g. `"$busy"` or a custom code). */
  readonly code: string;
  /** The builtin code, when `code` is one of the six builtin protocol codes. */
  readonly builtin?: ParleyBuiltinErrorCode;
  /** The typed custom error, when `code` is a consumer code. */
  readonly custom?: TError;
  /** Optional human-readable detail (the `m` field), when supplied by the handler. */
  readonly message?: string;
}

/**
 * The single terminal outcome the commander observes for a command: success with a typed result,
 * or an error. Every expected outcome — including all six builtin codes — is a value, never a
 * rejection (protocol.md §5, design.md).
 */
export type ParleyResult<TResult, TError> =
  | { readonly ok: true; readonly value: TResult }
  | { readonly ok: false; readonly error: ParleyError<TError> };

/** The kind tag for a {@link ParleyOutcome}. */
export type ParleyOutcomeKind = 'success' | 'error' | 'cancelled';

/**
 * The outcome a handler returns for one command: a success payload, a custom error, or cancellation
 * (emitted on the wire as `$cancelled`). Construct via the {@link ParleyOutcome} factories.
 */
export type ParleyOutcome<TResult, TError> =
  | { readonly kind: 'success'; readonly value: TResult }
  | { readonly kind: 'error'; readonly error: TError; readonly message?: string }
  | { readonly kind: 'cancelled' };

/** Factory functions for {@link ParleyOutcome} values. */
export const ParleyOutcome = {
  /** A successful outcome carrying a result payload. */
  success<TResult>(value: TResult): ParleyOutcome<TResult, never> {
    return { kind: 'success', value };
  },
  /** A custom-error outcome. The code must be a plain string not beginning with `$` (§5). */
  error<TError>(error: TError, message?: string): ParleyOutcome<never, TError> {
    return message === undefined ? { kind: 'error', error } : { kind: 'error', error, message };
  },
  /** A cancelled outcome, emitted on the wire as `$cancelled`. */
  cancelled(): ParleyOutcome<never, never> {
    return { kind: 'cancelled' };
  },
};
