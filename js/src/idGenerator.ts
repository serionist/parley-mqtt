/**
 * Command-id generator (protocol.md §4.2). Injectable per the design so constrained devices can
 * supply a cheaper source. Ids MUST be 1–64 chars from `[A-Za-z0-9_-]` and carry at least 64 bits
 * of randomness.
 */
export interface ParleyIdGenerator {
  /** Returns a fresh, valid command id. */
  newId(): string;
}

const BASE64URL = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';

/** URL-safe Base64 (no padding) of raw bytes — every output char is within `[A-Za-z0-9_-]`. */
function base64url(bytes: Uint8Array): string {
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i]!;
    const b1 = i + 1 < bytes.length ? bytes[i + 1]! : 0;
    const b2 = i + 2 < bytes.length ? bytes[i + 2]! : 0;
    out += BASE64URL[b0 >> 2];
    out += BASE64URL[((b0 & 0x03) << 4) | (b1 >> 4)];
    if (i + 1 < bytes.length) {
      out += BASE64URL[((b1 & 0x0f) << 2) | (b2 >> 6)];
    }
    if (i + 2 < bytes.length) {
      out += BASE64URL[b2 & 0x3f];
    }
  }
  return out;
}

function fillRandom(bytes: Uint8Array): void {
  const g = globalThis as { crypto?: { getRandomValues?: (array: Uint8Array) => Uint8Array } };
  if (g.crypto && typeof g.crypto.getRandomValues === 'function') {
    g.crypto.getRandomValues(bytes);
    return;
  }
  // Fallback for engines without Web Crypto. Weaker, but still yields a per-topic-set-unique id
  // at the sizes Parley uses; constrained devices SHOULD inject a better source.
  for (let i = 0; i < bytes.length; i++) {
    bytes[i] = Math.floor(Math.random() * 256);
  }
}

/**
 * The default generator: 128 bits of randomness (Web Crypto where present, else `Math.random`)
 * rendered as 22 URL-safe Base64 characters.
 */
export class DefaultIdGenerator implements ParleyIdGenerator {
  /** A shared instance (stateless). */
  static readonly instance = new DefaultIdGenerator();

  newId(): string {
    const bytes = new Uint8Array(16);
    fillRandom(bytes);
    return base64url(bytes);
  }
}
