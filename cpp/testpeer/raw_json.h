#ifndef PARLEY_TESTPEER_RAW_JSON_H
#define PARLEY_TESTPEER_RAW_JSON_H

#include <stddef.h>

#include "Parley.h"  // pulls detail/json.h first (ARDUINOJSON_USE_DOUBLE=1, POOL_CAPACITY) then ArduinoJson

// Raw-JSON payload binding for the interop peer: stores a value's serialized bytes and re-emits them
// verbatim, so ONE commander/handler instantiation round-trips every scenario's arbitrary payload
// (unicode, 3.14, nesting) with exact fidelity — the C++ analogue of the C#/Python/JS peers' raw
// pass-through. Buffer sized to match the peer's MaxPayload so a full echo payload fits.
static const size_t kRawPayloadCap = 1024;

struct RawPayload {
  char bytes[kRawPayloadCap];
  size_t len;
  bool present;

  RawPayload() : len(0), present(false) { bytes[0] = '\0'; }
};

// A custom error type that is exactly its wire code string.
struct RawCode {
  char code[64];
  RawCode() { code[0] = '\0'; }
};

namespace parley {

template <>
struct ParleyCodec<RawPayload> {
  static bool encode(const RawPayload& value, ArduinoJson::JsonVariant dst) {
    if (!value.present) return true;  // leave dst null → the envelope builder omits p
    return ArduinoJson::deserializeJson(dst, value.bytes, value.len) == ArduinoJson::DeserializationError::Ok;
  }
  static bool decode(ArduinoJson::JsonVariantConst src, RawPayload& out) {
    out.len = ArduinoJson::serializeJson(src, out.bytes, sizeof(out.bytes));
    out.present = true;
    return true;
  }
};

template <>
struct ParleyCodec<RawCode> {
  static bool encode(const RawCode& value, ArduinoJson::JsonVariant dst) { return dst.set(value.code); }
  static bool decode(ArduinoJson::JsonVariantConst src, RawCode& out) {
    const char* s = src.as<const char*>();
    if (s == nullptr) return false;
    size_t i = 0;
    for (; s[i] != '\0' && i + 1 < sizeof(out.code); ++i) out.code[i] = s[i];
    out.code[i] = '\0';
    return true;
  }
};

}  // namespace parley

#endif  // PARLEY_TESTPEER_RAW_JSON_H
