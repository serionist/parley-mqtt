#include "wire.h"

#include <string.h>

namespace parley {
namespace detail {
namespace wire {

using ArduinoJson::DeserializationError;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariant;
using ArduinoJson::JsonVariantConst;

namespace {

MsgType typeFromString(const char* t) {
  if (t == nullptr) return MsgType::Unknown;
  if (strcmp(t, "req") == 0) return MsgType::Req;
  if (strcmp(t, "hb") == 0) return MsgType::Hb;
  if (strcmp(t, "prg") == 0) return MsgType::Prg;
  if (strcmp(t, "res") == 0) return MsgType::Res;
  if (strcmp(t, "rak") == 0) return MsgType::Rak;
  if (strcmp(t, "cnl") == 0) return MsgType::Cnl;
  return MsgType::Unknown;
}

// Serializes `doc` into `buf`, returning the length or 0 on failure (overflowed / larger than cap).
size_t finish(JsonDocument& doc, uint8_t* buf, size_t cap) {
  if (doc.overflowed()) return 0;
  size_t needed = ArduinoJson::measureJson(doc);
  if (needed > cap) return 0;
  size_t written = ArduinoJson::serializeJson(doc, buf, cap);
  return written;
}

// Common header: {"v":1,"t":<t>,"id":<id>, ...}. The id/t are borrowed const char* — valid for the
// life of this build call, so ArduinoJson links (does not copy) them.
void header(JsonDocument& doc, const char* t, const char* id) {
  doc["v"] = 1;
  doc["t"] = t;
  doc["id"] = id;
}

}  // namespace

bool isValidId(const char* id) {
  if (id == nullptr) return false;
  size_t n = 0;
  for (const char* p = id; *p != '\0'; ++p) {
    char c = *p;
    bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-';
    if (!ok) return false;
    if (++n > 64) return false;
  }
  return n >= 1;
}

bool parse(JsonDocument& doc, FixedArena& arena, const uint8_t* bytes, size_t len, Parsed& out) {
  arena.reset();
  doc.clear();
  DeserializationError err = ArduinoJson::deserializeJson(doc, bytes, len);
  if (err != DeserializationError::Ok) return false;
  if (!doc.is<JsonObjectConst>()) return false;  // §4: must be a JSON object

  JsonObjectConst obj = doc.as<JsonObjectConst>();

  JsonVariantConst vv = obj["v"];
  out.v = vv.is<int>() ? vv.as<int>() : -1;

  out.t = typeFromString(obj["t"].as<const char*>());
  out.id = obj["id"].as<const char*>();

  out.hasP = !obj["p"].isNull();
  out.p = obj["p"];

  JsonVariantConst okv = obj["ok"];
  out.hasOk = okv.is<bool>();
  out.ok = out.hasOk ? okv.as<bool>() : false;

  out.e = obj["e"].as<const char*>();
  out.m = obj["m"].as<const char*>();
  return true;
}

size_t buildRequest(uint8_t* buf, size_t cap, FixedArena& arena, const char* id, PayloadWriter w,
                    void* ctx) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "req", id);
  JsonVariant p = doc["p"].to<JsonVariant>();
  if (!w(ctx, p)) return 0;
  if (p.isNull()) doc.remove("p");  // §4: null payload ≡ absent
  return finish(doc, buf, cap);
}

size_t buildProgress(uint8_t* buf, size_t cap, FixedArena& arena, const char* id, PayloadWriter w,
                     void* ctx) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "prg", id);
  JsonVariant p = doc["p"].to<JsonVariant>();
  if (!w(ctx, p)) return 0;
  if (p.isNull()) doc.remove("p");
  return finish(doc, buf, cap);
}

size_t buildSuccessResult(uint8_t* buf, size_t cap, FixedArena& arena, const char* id, PayloadWriter w,
                          void* ctx) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "res", id);
  doc["ok"] = true;
  JsonVariant p = doc["p"].to<JsonVariant>();
  if (!w(ctx, p)) return 0;
  if (p.isNull()) doc.remove("p");
  return finish(doc, buf, cap);
}

size_t buildCustomErrorResult(uint8_t* buf, size_t cap, FixedArena& arena, const char* id,
                              const char* code, const char* m) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "res", id);
  doc["ok"] = false;
  doc["e"] = code;
  if (m != nullptr) doc["m"] = m;
  return finish(doc, buf, cap);
}

size_t buildHeartbeat(uint8_t* buf, size_t cap, FixedArena& arena, const char* id) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "hb", id);
  return finish(doc, buf, cap);
}

size_t buildResultAck(uint8_t* buf, size_t cap, FixedArena& arena, const char* id) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "rak", id);
  return finish(doc, buf, cap);
}

size_t buildCancel(uint8_t* buf, size_t cap, FixedArena& arena, const char* id) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "cnl", id);
  return finish(doc, buf, cap);
}

size_t buildBuiltinErrorResult(uint8_t* buf, size_t cap, FixedArena& arena, const char* id,
                               const char* code) {
  arena.reset();
  JsonDocument doc(&arena);
  header(doc, "res", id);
  doc["ok"] = false;
  doc["e"] = code;
  return finish(doc, buf, cap);
}

}  // namespace wire
}  // namespace detail
}  // namespace parley
