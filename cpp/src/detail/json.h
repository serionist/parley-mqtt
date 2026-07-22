#ifndef PARLEY_DETAIL_JSON_H
#define PARLEY_DETAIL_JSON_H

// The single include site for ArduinoJson across Parley. It pins ARDUINOJSON_USE_DOUBLE=1 (plan F3)
// so IEEE-754 doubles like 3.14 round-trip exactly on every target — including 64-bit hosts, where
// ArduinoJson would otherwise still default to double, and 8/16-bit MCUs, where it would default to
// float. The native conformance build also sets this via the compiler (-DARDUINOJSON_USE_DOUBLE=1);
// defining it here first guarantees it for Arduino/PlatformIO consumers too. Every Parley header
// includes THIS, never <ArduinoJson.h> directly, so the macro is always seen before the library.

#ifndef ARDUINOJSON_USE_DOUBLE
#define ARDUINOJSON_USE_DOUBLE 1
#endif

// Bound the variant-pool block size. ArduinoJson v7 allocates variant slots in fixed blocks of
// ARDUINOJSON_POOL_CAPACITY slots (default 256 on 32/64-bit → a 1–4 KB block per document, even a
// tiny one). A small block keeps the fixed-arena footprint low and predictable — a document parsed
// from N bytes then needs O(N) arena, so the shell's arena (a modest multiple of MaxPayload) is
// comfortably sufficient on every target. 16 slots trades a little per-block overhead on large
// documents for a tight, deterministic memory profile — exactly the constrained-target tradeoff.
#ifndef ARDUINOJSON_POOL_CAPACITY
#define ARDUINOJSON_POOL_CAPACITY 16
#endif

#include <ArduinoJson.h>

#endif  // PARLEY_DETAIL_JSON_H
