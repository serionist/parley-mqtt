/**
 * Deep JSON equality up to object key order (protocol.md §10 exact-set matching): objects compare
 * by key set and values, arrays element-wise in order, numbers by value, everything else exact.
 * Both operands are values produced by `JSON.parse`, so `undefined` never appears inside them.
 */
export function jsonEqual(a: unknown, b: unknown): boolean {
  if (a === b) {
    return true;
  }
  if (a === null || b === null) {
    return a === b;
  }

  const ta = typeof a;
  const tb = typeof b;
  if (ta !== tb) {
    return false;
  }

  if (ta === 'number') {
    return (a as number) === (b as number);
  }
  if (ta !== 'object') {
    // string / boolean already handled by ===; anything else compares by identity.
    return a === b;
  }

  const aArray = Array.isArray(a);
  const bArray = Array.isArray(b);
  if (aArray || bArray) {
    if (!aArray || !bArray) {
      return false;
    }
    const arrA = a as unknown[];
    const arrB = b as unknown[];
    if (arrA.length !== arrB.length) {
      return false;
    }
    for (let i = 0; i < arrA.length; i++) {
      if (!jsonEqual(arrA[i], arrB[i])) {
        return false;
      }
    }
    return true;
  }

  const objA = a as Record<string, unknown>;
  const objB = b as Record<string, unknown>;
  const keysA = Object.keys(objA);
  const keysB = Object.keys(objB);
  if (keysA.length !== keysB.length) {
    return false;
  }
  for (const key of keysA) {
    if (!Object.prototype.hasOwnProperty.call(objB, key)) {
      return false;
    }
    if (!jsonEqual(objA[key], objB[key])) {
      return false;
    }
  }
  return true;
}
