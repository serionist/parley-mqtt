#ifndef PARLEY_DETAIL_WIRE_H
#define PARLEY_DETAIL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "fixed_arena.h"
#include "json.h"

// Envelope (de)serialization (protocol.md §4) over ArduinoJson with fixed arenas. Non-templated: the
// whole state machine shares one compiled copy regardless of the consumer's payload types (plan F9).
// Typed payloads cross this boundary as ArduinoJson variants, never as the C++ payload types.

namespace parley {
namespace detail {
namespace wire {

enum class MsgType { Unknown, Req, Hb, Prg, Res, Rak, Cnl };

// protocol.md §4.2: an opaque string, 1–64 chars from [A-Za-z0-9_-].
bool isValidId(const char* id);

// A parsed inbound envelope. Pointer/variant members are valid only while the JsonDocument passed to
// parse() stays alive.
struct Parsed {
  int v;
  MsgType t;
  const char* id;   // nullptr if absent / not a string
  bool hasP;        // p present AND non-null (protocol.md §4: omitted/null ≡ no payload)
  ArduinoJson::JsonVariantConst p;
  bool hasOk;       // ok present AND a JSON boolean
  bool ok;
  const char* e;    // nullptr if absent / not a string
  const char* m;    // nullptr if absent / not a string
};

// Parses `len` bytes into `doc` (backed by `arena`). Returns true iff the bytes are a JSON object that
// fit the arena; v/t/id/required-field validation is the caller's, per the §4 ignore rules.
bool parse(ArduinoJson::JsonDocument& doc, FixedArena& arena, const uint8_t* bytes, size_t len,
           Parsed& out);

// Fills `dst` with a typed payload; returns false on encode failure (arena overflow / codec failure).
typedef bool (*PayloadWriter)(void* ctx, ArduinoJson::JsonVariant dst);

// Builders serialize into `buf` (cap bytes) through a JsonDocument backed by `arena`, returning the
// serialized length, or 0 on failure (writer returned false / doc overflowed / serialized size > cap).
size_t buildRequest(uint8_t* buf, size_t cap, FixedArena& arena, const char* id, PayloadWriter w,
                    void* ctx);
size_t buildProgress(uint8_t* buf, size_t cap, FixedArena& arena, const char* id, PayloadWriter w,
                     void* ctx);
size_t buildSuccessResult(uint8_t* buf, size_t cap, FixedArena& arena, const char* id, PayloadWriter w,
                          void* ctx);
size_t buildCustomErrorResult(uint8_t* buf, size_t cap, FixedArena& arena, const char* id,
                              const char* code, const char* m /* nullable */);

// Payload-free / fixed-shape envelopes.
size_t buildHeartbeat(uint8_t* buf, size_t cap, FixedArena& arena, const char* id);
size_t buildResultAck(uint8_t* buf, size_t cap, FixedArena& arena, const char* id);
size_t buildCancel(uint8_t* buf, size_t cap, FixedArena& arena, const char* id);
size_t buildBuiltinErrorResult(uint8_t* buf, size_t cap, FixedArena& arena, const char* id,
                               const char* code);

}  // namespace wire
}  // namespace detail
}  // namespace parley

#endif  // PARLEY_DETAIL_WIRE_H
