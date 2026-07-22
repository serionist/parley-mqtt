#ifndef PARLEY_TEST_RAW_JSON_H
#define PARLEY_TEST_RAW_JSON_H

#include <stddef.h>
#include <string.h>

#include "ParleyCodec.h"
#include "detail/json.h"

// A payload type for the vector runner that round-trips ANY JSON value verbatim: it stores the value's
// serialized bytes and re-emits them. This lets one commander/handler instantiation replay every
// vector's arbitrary request/progress/result payloads (unicode, 3.14, nesting) with exact fidelity —
// the C++ analogue of the C# runner's raw JsonElement binding.
struct JsonPayload {
  char bytes[256];
  size_t len;
  bool present;

  JsonPayload() : len(0), present(false) { bytes[0] = '\0'; }
};

// A custom error type for the runner that is exactly its wire code string.
struct RawCode {
  char code[48];
  RawCode() { code[0] = '\0'; }
};

namespace parley {

template <>
struct ParleyCodec<JsonPayload> {
  static bool encode(const JsonPayload& value, ArduinoJson::JsonVariant dst) {
    if (!value.present) return true;  // leave dst null → the envelope builder omits p
    return ArduinoJson::deserializeJson(dst, value.bytes, value.len) == ArduinoJson::DeserializationError::Ok;
  }
  static bool decode(ArduinoJson::JsonVariantConst src, JsonPayload& out) {
    size_t n = ArduinoJson::serializeJson(src, out.bytes, sizeof(out.bytes));
    out.len = n;
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

#endif  // PARLEY_TEST_RAW_JSON_H
