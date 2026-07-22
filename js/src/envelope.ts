/**
 * Envelope codec (protocol.md §4) plus the §4/§4.2 robustness checks. The codec knows nothing about
 * typed payloads; the payload `p` crosses the boundary as a raw JSON value.
 */

/** The single protocol version this document defines. */
export const PROTOCOL_VERSION = 1;

/** Wire message-type strings (protocol.md §4). */
export const MessageType = {
  Request: 'req',
  Heartbeat: 'hb',
  Progress: 'prg',
  Result: 'res',
  ResultAck: 'rak',
  Cancel: 'cnl',
} as const;

export type MessageTypeValue = (typeof MessageType)[keyof typeof MessageType];

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: false });

/** A parsed inbound envelope. `p` is the raw JSON payload value (absent → `undefined`). */
export interface InboundEnvelope {
  readonly v: number;
  readonly t: string;
  readonly id: string;
  /** Raw payload value; `undefined` when the `p` field is absent (null is normalized to undefined). */
  readonly p: unknown;
  /** `ok` when present as a boolean (only on `res`); otherwise `undefined`. */
  readonly ok: boolean | undefined;
  /** `e` when present as a string; otherwise `undefined`. */
  readonly e: string | undefined;
  /** `m` when present as a string; otherwise `undefined`. */
  readonly m: string | undefined;
}

/** Fields for an outbound envelope; omitted (`undefined`) fields are not serialized. */
export interface OutboundEnvelope {
  v: number;
  t: string;
  id: string;
  p?: unknown;
  ok?: boolean;
  e?: string;
  m?: string;
}

/** Serializes an outbound envelope to UTF-8 JSON bytes. Absent fields are omitted. */
export function serialize(env: OutboundEnvelope): Uint8Array {
  const out: Record<string, unknown> = { v: env.v, t: env.t, id: env.id };
  if (env.p !== undefined) {
    out.p = env.p;
  }
  if (env.ok !== undefined) {
    out.ok = env.ok;
  }
  if (env.e !== undefined) {
    out.e = env.e;
  }
  if (env.m !== undefined) {
    out.m = env.m;
  }
  return encoder.encode(JSON.stringify(out));
}

/**
 * Parses payload bytes into an {@link InboundEnvelope}, applying §4 robustness at the envelope level:
 * returns `null` for bytes that are not a JSON object, or that lack a required field (`v` integer,
 * `t` string, `id` string). Unknown extra fields are ignored (forward compatibility). Per-type
 * conditional requirements (`ok` on `res`; `e` when `ok:false`) and §4.2 id validity are checked by
 * the caller.
 */
export function parse(bytes: Uint8Array): InboundEnvelope | null {
  let text: string;
  try {
    text = decoder.decode(bytes);
  } catch {
    return null;
  }

  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return null;
  }

  // Must be a JSON object (not an array, scalar, or null).
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return null;
  }

  const obj = value as Record<string, unknown>;
  const v = obj.v;
  const t = obj.t;
  const id = obj.id;

  if (typeof v !== 'number' || !Number.isInteger(v)) {
    return null;
  }
  if (typeof t !== 'string') {
    return null;
  }
  if (typeof id !== 'string') {
    return null;
  }

  // "omitted/null means no payload" (§4): normalize a present-null payload to absent.
  const rawP = obj.p;
  const p = rawP === null ? undefined : rawP;

  return {
    v,
    t,
    id,
    p,
    ok: typeof obj.ok === 'boolean' ? obj.ok : undefined,
    e: typeof obj.e === 'string' ? obj.e : undefined,
    m: typeof obj.m === 'string' ? obj.m : undefined,
  };
}

/** Validates a command id against protocol.md §4.2 (1–64 chars from `[A-Za-z0-9_-]`). */
export function isValidId(id: string): boolean {
  const len = id.length;
  if (len < 1 || len > 64) {
    return false;
  }
  for (let i = 0; i < len; i++) {
    const c = id.charCodeAt(i);
    const ok =
      (c >= 65 && c <= 90) || // A-Z
      (c >= 97 && c <= 122) || // a-z
      (c >= 48 && c <= 57) || // 0-9
      c === 95 || // _
      c === 45; // -
    if (!ok) {
      return false;
    }
  }
  return true;
}

/**
 * Normalizes a typed payload for the `p` field: a `null`/`undefined` value maps to an absent payload
 * (protocol.md §4: "omitted/null means no payload"). Any other value passes through as raw JSON.
 */
export function toPayload(value: unknown): unknown {
  return value === null || value === undefined ? undefined : value;
}
