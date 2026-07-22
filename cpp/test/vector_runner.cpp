#include "vector_runner.h"

#include <string.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "Parley.h"
#include "doctest.h"
#include "fake_adapter.h"
#include "json_compare.h"
#include "raw_json.h"

using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;
using parley::ParleyCallHandle;
using parley::ParleyCallOptions;
using parley::ParleyConfig;
using parley::ParleyExecution;
using parley::ParleyResult;
using parley::ParleyTopics;

// Runner pool sizing: MaxPayload 512 holds every vector envelope; MaxInFlight 4 covers the multi-call
// vectors; handlers instantiate at MaxCommands 1 (maxActiveCommands:1 vectors — the pool IS the cap)
// or 32 (default). These are the runner's chosen test caps (plan §8 risk item).
static const int kMaxPayload = 512;

namespace {

uint32_t getU32(JsonObjectConst o, const char* key, uint32_t def) {
  JsonVariantConst v = o[key];
  if (v.is<long long>()) return static_cast<uint32_t>(v.as<long long>());
  return def;
}

int getInt(JsonObjectConst o, const char* key, int def) {
  JsonVariantConst v = o[key];
  if (v.is<long long>()) return static_cast<int>(v.as<long long>());
  return def;
}

bool objHasKey(JsonObjectConst o, const char* key) {
  for (JsonPairConst kv : o) {
    if (strcmp(kv.key().c_str(), key) == 0) return true;
  }
  return false;
}

ParleyConfig buildConfig(JsonObjectConst c) {
  ParleyConfig cfg;  // defaults (protocol.md §8)
  cfg.requestRetryInterval = getU32(c, "requestRetryInterval", cfg.requestRetryInterval);
  cfg.noAckTimeout = getU32(c, "noAckTimeout", cfg.noAckTimeout);
  cfg.staleTimeout = getU32(c, "staleTimeout", cfg.staleTimeout);
  cfg.executionTimeout = getU32(c, "executionTimeout", cfg.executionTimeout);
  cfg.cancelRetryInterval = getU32(c, "cancelRetryInterval", cfg.cancelRetryInterval);
  cfg.heartbeatInterval = getU32(c, "heartbeatInterval", cfg.heartbeatInterval);
  cfg.resultRetryInterval = getU32(c, "resultRetryInterval", cfg.resultRetryInterval);
  cfg.resultRetryTimeout = getU32(c, "resultRetryTimeout", cfg.resultRetryTimeout);
  cfg.tombstoneTtl = getU32(c, "tombstoneTtl", cfg.tombstoneTtl);
  cfg.maxConcurrentExecutions = getInt(c, "maxConcurrentExecutions", cfg.maxConcurrentExecutions);
  return cfg;
}

ParleyCallOptions buildOptions(JsonObjectConst o) {
  ParleyCallOptions opt;
  if (o.isNull()) return opt;
  opt.requestRetryInterval = getU32(o, "requestRetryInterval", 0);
  opt.noAckTimeout = getU32(o, "noAckTimeout", 0);
  opt.staleTimeout = getU32(o, "staleTimeout", 0);
  opt.executionTimeout = getU32(o, "executionTimeout", 0);
  opt.cancelRetryInterval = getU32(o, "cancelRetryInterval", 0);
  opt.tombstoneTtl = getU32(o, "tombstoneTtl", 0);
  return opt;
}

JsonPayload rawFromVariant(JsonVariantConst v) {
  JsonPayload r;
  if (v.isNull()) return r;  // absent/null → no payload
  r.len = ArduinoJson::serializeJson(v, r.bytes, sizeof(r.bytes));
  r.present = true;
  return r;
}

std::string variantToString(JsonVariantConst v) {
  char buf[1024];
  size_t n = ArduinoJson::serializeJson(v, buf, sizeof(buf));
  return std::string(buf, n);
}

std::string describeAction(const OutboundAction& a) {
  if (a.isSubscribe) return "{subscribe: " + a.topic + "}";
  std::string s(a.payload.begin(), a.payload.end());
  return "{topic: " + a.topic + ", msg: " + s + "}";
}

template <class T>
void deliverInbound(T& obj, JsonObjectConst inbound) {
  const char* topic = inbound["topic"];
  if (!inbound["raw"].isNull()) {
    const char* raw = inbound["raw"];
    obj.deliver(topic, reinterpret_cast<const uint8_t*>(raw), strlen(raw));
  } else {
    JsonVariantConst msg = inbound["msg"];
    uint8_t buf[2048];
    size_t n = ArduinoJson::serializeJson(msg, buf, sizeof(buf));
    obj.deliver(topic, buf, n);
  }
}

void ensureSingleDirective(const std::string& name, JsonObjectConst step) {
  static const char* kKeys[] = {"call", "cancel", "in", "handler", "connected", "expect"};
  int count = 0;
  for (const char* k : kKeys) {
    if (objHasKey(step, k)) ++count;
  }
  CHECK_MESSAGE(count == 1,
                "vector '" << name << "': each step must carry exactly one directive, found " << count);
}

void assertExpect(const std::string& name, uint32_t at, FakeAdapter& adapter, JsonArrayConst expected) {
  std::vector<bool> used(adapter.outbound.size(), false);
  for (JsonVariantConst e : expected) {
    JsonObjectConst exp = e.as<JsonObjectConst>();
    int idx = -1;
    if (!exp["subscribe"].isNull()) {
      const char* topic = exp["subscribe"];
      for (size_t i = 0; i < adapter.outbound.size(); ++i) {
        if (!used[i] && adapter.outbound[i].isSubscribe && adapter.outbound[i].topic == topic) {
          idx = static_cast<int>(i);
          break;
        }
      }
    } else {
      const char* topic = exp["topic"];
      JsonVariantConst msg = exp["msg"];
      for (size_t i = 0; i < adapter.outbound.size(); ++i) {
        const OutboundAction& a = adapter.outbound[i];
        if (used[i] || a.isSubscribe || a.topic != topic) continue;
        JsonDocument actual;
        if (ArduinoJson::deserializeJson(actual, a.payload.data(), a.payload.size()) !=
            ArduinoJson::DeserializationError::Ok) {
          continue;
        }
        if (jsonEqual(actual.as<JsonVariantConst>(), msg)) {
          idx = static_cast<int>(i);
          break;
        }
      }
    }
    CHECK_MESSAGE(idx >= 0, "vector '" << name << "' at t=" << at
                                       << ": expected outbound not found: " << variantToString(exp));
    if (idx >= 0) used[idx] = true;
  }
  for (size_t i = 0; i < adapter.outbound.size(); ++i) {
    CHECK_MESSAGE(used[i], "vector '" << name << "' at t=" << at << ": unexpected extra outbound: "
                                      << describeAction(adapter.outbound[i]));
  }
}

// --- commander role ---

struct CallRecord {
  ParleyResult<JsonPayload, RawCode> result;
  bool done;
  ParleyCallHandle handle;
  CallRecord() : done(false) {}
};

void onCommanderComplete(void* ud, const ParleyResult<JsonPayload, RawCode>& r) {
  CallRecord* rec = static_cast<CallRecord*>(ud);
  rec->result = r;
  rec->done = true;
}

void assertOutcomes(const std::string& name, JsonObjectConst outcomes,
                    std::map<std::string, CallRecord>& calls) {
  // Every started call must have a declared expected outcome and have reached a terminal.
  for (std::map<std::string, CallRecord>::iterator it = calls.begin(); it != calls.end(); ++it) {
    CHECK_MESSAGE(objHasKey(outcomes, it->first.c_str()),
                  "vector '" << name << "': call '" << it->first << "' started but no outcome declared");
    CHECK_MESSAGE(it->second.done,
                  "vector '" << name << "': call '" << it->first << "' did not reach a terminal");
  }
  for (JsonPairConst kv : outcomes) {
    std::string id = kv.key().c_str();
    JsonObjectConst exp = kv.value().as<JsonObjectConst>();
    std::map<std::string, CallRecord>::iterator it = calls.find(id);
    CHECK_MESSAGE(it != calls.end(),
                  "vector '" << name << "': outcome for '" << id << "' that was never started");
    if (it == calls.end()) continue;
    const CallRecord& rec = it->second;
    if (!rec.done) continue;
    bool expOk = exp["ok"].as<bool>();
    if (expOk) {
      CHECK_MESSAGE(rec.result.isSuccess(),
                    "vector '" << name << "': call '" << id << "' expected success, got error '"
                               << (rec.result.isError() ? rec.result.error().code() : "?") << "'");
      if (!rec.result.isSuccess()) continue;
      bool expHasP = objHasKey(exp, "p");
      if (expHasP) {
        CHECK_MESSAGE(rec.result.value().present,
                      "vector '" << name << "': call '" << id << "' expected a payload but got none");
        if (rec.result.value().present) {
          JsonDocument actual;
          ArduinoJson::deserializeJson(actual, rec.result.value().bytes, rec.result.value().len);
          CHECK_MESSAGE(jsonEqual(actual.as<JsonVariantConst>(), exp["p"]),
                        "vector '" << name << "': call '" << id << "' payload mismatch. expected "
                                   << variantToString(exp["p"]) << ", got "
                                   << std::string(rec.result.value().bytes, rec.result.value().len));
        }
      } else {
        CHECK_MESSAGE(!rec.result.value().present, "vector '" << name << "': call '" << id
                                                              << "' expected no payload but got one");
      }
    } else {
      CHECK_MESSAGE(rec.result.isError(),
                    "vector '" << name << "': call '" << id << "' expected error but succeeded");
      if (!rec.result.isError()) continue;
      const char* expE = exp["e"];
      CHECK_MESSAGE(strcmp(rec.result.error().code(), expE) == 0,
                    "vector '" << name << "': call '" << id << "' error code mismatch. expected '"
                               << expE << "', got '" << rec.result.error().code() << "'");
    }
  }
}

void runCommander(const std::string& name, JsonObjectConst config, JsonArrayConst steps,
                  JsonObjectConst outcomes) {
  FakeAdapter adapter;
  ScriptedIdGenerator idgen;
  ParleyConfig cfg = buildConfig(config);
  ParleyTopics topics("request", "heartbeat", "progress", "result");
  parley::ParleyCommander<JsonPayload, JsonPayload, JsonPayload, RawCode, 4, kMaxPayload> commander(adapter, topics,
                                                                                        cfg, &idgen);
  adapter.clear();  // §10: construction-time subscribes are not asserted

  std::map<std::string, CallRecord> calls;
  uint32_t lastAt = 0;
  bool firstStep = true;

  for (JsonVariantConst stepV : steps) {
    JsonObjectConst step = stepV.as<JsonObjectConst>();
    uint32_t at = static_cast<uint32_t>(step["at"].as<long long>());
    CHECK_MESSAGE(step["at"].is<long long>(), "vector '" << name << "': step 'at' must be an integer");
    ensureSingleDirective(name, step);
    // TQ1: tick exactly once per distinct `at`, mirroring the C# runner (which only advances the
    // virtual clock on a positive delta). Re-ticking at the same `at` before a later same-`at` expect
    // would mask a defect that publishes from tick() rather than synchronously.
    if (firstStep || at != lastAt) commander.tick(at);  // §10: all timers due <= at fire before step
    firstStep = false;
    lastAt = at;

    if (!step["call"].isNull()) {
      JsonObjectConst call = step["call"].as<JsonObjectConst>();
      std::string id = call["id"].as<const char*>();
      idgen.enqueue(id);
      JsonPayload req = rawFromVariant(call["p"]);
      ParleyCallOptions opts = buildOptions(call["options"].as<JsonObjectConst>());
      CallRecord& rec = calls[id];
      rec.handle = commander.execute(req, opts, nullptr, &onCommanderComplete, &rec);
    } else if (!step["cancel"].isNull()) {
      std::string id = step["cancel"].as<const char*>();
      std::map<std::string, CallRecord>::iterator it = calls.find(id);
      CHECK_MESSAGE(it != calls.end(), "vector '" << name << "': cancel of unknown call '" << id << "'");
      if (it != calls.end()) commander.cancel(it->second.handle);
    } else if (!step["in"].isNull()) {
      deliverInbound(commander, step["in"].as<JsonObjectConst>());
    } else if (!step["connected"].isNull()) {
      commander.connected();
    } else if (!step["expect"].isNull()) {
      assertExpect(name, at, adapter, step["expect"].as<JsonArrayConst>());
      adapter.clear();
    }
  }

  CHECK_MESSAGE(adapter.outbound.empty(), "vector '" << name << "' at t=" << lastAt
                                                     << ": outbound left unasserted after final step: "
                                                     << adapter.outbound.size() << " action(s)");
  assertOutcomes(name, outcomes, calls);
}

// --- handler role ---

template <int N>
class VectorHandler : public parley::ParleyCommandHandler<JsonPayload, JsonPayload, JsonPayload, RawCode, N,
                                                          kMaxPayload> {
 public:
  typedef parley::ParleyCommandHandler<JsonPayload, JsonPayload, JsonPayload, RawCode, N, kMaxPayload> Base;
  typedef ParleyExecution<JsonPayload, JsonPayload, JsonPayload, RawCode> Exec;

  VectorHandler(parley::ParleyConnectionAdapter& a, const ParleyTopics& t, const ParleyConfig& c)
      : Base(a, t, c) {}

  void doComplete(const std::string& id, const JsonPayload& p) { pending_.at(id)->succeed(p); }
  void doProgress(const std::string& id, const JsonPayload& p) { pending_.at(id)->reportProgress(p); }
  void doFail(const std::string& id, const std::string& code) {
    // The scripted-fail mapper routes builtin $-codes (at minimum $cancelled) through the builtin
    // path, never through exec.fail(Err) (plan F4).
    if (code == "$cancelled") {
      pending_.at(id)->cancel();
    } else {
      RawCode rc;
      size_t i = 0;
      for (; i < code.size() && i + 1 < sizeof(rc.code); ++i) rc.code[i] = code[i];
      rc.code[i] = '\0';
      pending_.at(id)->fail(rc);
    }
  }

 protected:
  void handleCommand(Exec& execution) override { pending_[execution.commandId()] = &execution; }

 private:
  std::map<std::string, Exec*> pending_;
};

template <int N>
void runHandler(const std::string& name, JsonObjectConst config, JsonArrayConst steps) {
  FakeAdapter adapter;
  ParleyConfig cfg = buildConfig(config);
  ParleyTopics topics("request", "heartbeat", "progress", "result");
  VectorHandler<N> handler(adapter, topics, cfg);
  adapter.clear();

  uint32_t lastAt = 0;
  bool firstStep = true;
  for (JsonVariantConst stepV : steps) {
    JsonObjectConst step = stepV.as<JsonObjectConst>();
    uint32_t at = static_cast<uint32_t>(step["at"].as<long long>());
    CHECK_MESSAGE(step["at"].is<long long>(), "vector '" << name << "': step 'at' must be an integer");
    ensureSingleDirective(name, step);
    // TQ1: tick exactly once per distinct `at` (see runCommander).
    if (firstStep || at != lastAt) handler.tick(at);
    firstStep = false;
    lastAt = at;

    if (!step["in"].isNull()) {
      deliverInbound(handler, step["in"].as<JsonObjectConst>());
    } else if (!step["handler"].isNull()) {
      JsonObjectConst h = step["handler"].as<JsonObjectConst>();
      std::string id = h["id"].as<const char*>();
      std::string action = h["action"].as<const char*>();
      if (action == "complete") {
        handler.doComplete(id, rawFromVariant(h["p"]));
      } else if (action == "fail") {
        handler.doFail(id, std::string(h["e"].as<const char*>()));
      } else if (action == "progress") {
        handler.doProgress(id, rawFromVariant(h["p"]));
      } else {
        FAIL_CHECK("vector '" << name << "': unknown handler action '" << action << "'");
      }
    } else if (!step["connected"].isNull()) {
      handler.connected();
    } else if (!step["expect"].isNull()) {
      assertExpect(name, at, adapter, step["expect"].as<JsonArrayConst>());
      adapter.clear();
    }
  }

  CHECK_MESSAGE(adapter.outbound.empty(), "vector '" << name << "' at t=" << lastAt
                                                     << ": outbound left unasserted after final step: "
                                                     << adapter.outbound.size() << " action(s)");
}

}  // namespace

std::string vectorStructureError(const std::string& text) {
  // TQ2: top-level structural validation with NO doctest side effects, so it can gate runVector() AND
  // be asserted directly on synthetic malformed input. Guards the three UB/vacuous-pass traps: a
  // missing `role` (std::string(nullptr)), a missing `steps` (vacuous pass), and a decreasing `at`
  // (silent backward tick).
  JsonDocument doc;
  if (ArduinoJson::deserializeJson(doc, text) != ArduinoJson::DeserializationError::Ok)
    return "parse error";
  if (!doc.is<JsonObjectConst>()) return "root is not a JSON object";
  JsonObjectConst root = doc.as<JsonObjectConst>();

  JsonVariantConst roleV = root["role"];
  if (!roleV.is<const char*>()) return "missing or non-string 'role'";
  std::string role = roleV.as<const char*>();
  if (role != "commander" && role != "handler") return "unknown role '" + role + "'";

  JsonVariantConst stepsV = root["steps"];
  if (!stepsV.is<JsonArrayConst>()) return "missing 'steps' array";

  bool first = true;
  long long lastAt = 0;
  for (JsonVariantConst sv : stepsV.as<JsonArrayConst>()) {
    if (!sv.is<JsonObjectConst>()) return "step is not an object";
    JsonVariantConst atV = sv.as<JsonObjectConst>()["at"];
    if (!atV.is<long long>()) return "step 'at' must be an integer";
    long long at = atV.as<long long>();
    if (!first && at < lastAt) return "step 'at' must be non-decreasing";
    first = false;
    lastAt = at;
  }
  return "";
}

void runVector(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();

  // TQ2: reject malformed vectors up front so replay never hits UB (e.g. std::string(nullptr) on a
  // missing role). A real conformance vector always passes; a broken one fails loudly here.
  std::string structErr = vectorStructureError(text);
  REQUIRE_MESSAGE(structErr.empty(), "malformed vector " << path << ": " << structErr);
  if (!structErr.empty()) return;

  JsonDocument doc;
  ArduinoJson::DeserializationError err = ArduinoJson::deserializeJson(doc, text);
  REQUIRE_MESSAGE(err == ArduinoJson::DeserializationError::Ok,
                  "failed to parse vector file: " << path);
  if (err != ArduinoJson::DeserializationError::Ok) return;

  JsonObjectConst root = doc.as<JsonObjectConst>();
  std::string name = root["name"].is<const char*>() ? root["name"].as<const char*>() : path;
  std::string role = root["role"].as<const char*>();
  JsonObjectConst config = root["config"].as<JsonObjectConst>();
  JsonArrayConst steps = root["steps"].as<JsonArrayConst>();

  if (role == "commander") {
    runCommander(name, config, steps, root["outcomes"].as<JsonObjectConst>());
  } else if (role == "handler") {
    // The C++ pool IS the admission cap (design.md); pick the instantiation matching the vector's
    // maxActiveCommands (only 1 or the default appear across the 41 vectors).
    int maxActive = getInt(config, "maxActiveCommands", 32);
    if (maxActive <= 1) {
      runHandler<1>(name, config, steps);
    } else {
      runHandler<32>(name, config, steps);
    }
  } else {
    FAIL_CHECK("vector '" << name << "': unknown role '" << role << "'");
  }
}

std::vector<std::string> discoverVectors() {
  std::vector<std::string> files;
  std::error_code ec;
  std::filesystem::path dir(PARLEY_CONFORMANCE_DIR);
  for (std::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".json") {
      files.push_back(it->path().string());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}
