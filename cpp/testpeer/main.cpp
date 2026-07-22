// Parley layer-3 interop test peer (C++ / desktop libmosquitto), a GENERIC interpreter of
// docs/conformance/interop/scenarios.json. Handler role serves the five fixed harness command
// behaviours and prints READY after all SUBACKs; commander role runs the step model, writes the JSON
// report, and exits nonzero on any failure. Zero suite logic — only the harness.
//
// The MQTT client is libmosquitto, pumped SYNCHRONOUSLY: mosquitto_loop() runs in the peer's own main
// loop beside tick() (no threads, no mutex queue) — exactly the Arduino loop() model the tick-driven
// core is built for. No exceptions / no RTTI, matching the frozen core's discipline. QoS 0,
// retain=false (protocol.md §2 baseline).

#include <mosquitto.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "Parley.h"
#include "json_util.h"
#include "raw_json.h"

using ArduinoJson::JsonArray;
using ArduinoJson::JsonArrayConst;
using ArduinoJson::JsonDocument;
using ArduinoJson::JsonObject;
using ArduinoJson::JsonObjectConst;
using ArduinoJson::JsonVariantConst;
using parley::ParleyCallHandle;
using parley::ParleyCallOptions;
using parley::ParleyConfig;
using parley::ParleyConnectionAdapter;
using parley::ParleyExecution;
using parley::ParleyResult;
using parley::ParleyTopics;

static const int kMaxPayload = static_cast<int>(kRawPayloadCap);
static const char* kSelfLang = "cpp";

typedef parley::ParleyCommander<RawPayload, RawPayload, RawPayload, RawCode, 8, kMaxPayload> PeerCommander;

// ---------------------------------------------------------------------------
// clock
// ---------------------------------------------------------------------------
static uint32_t nowMs() {
  static const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  std::chrono::steady_clock::duration d = std::chrono::steady_clock::now() - t0;
  return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

// ---------------------------------------------------------------------------
// routing / callback context
// ---------------------------------------------------------------------------
typedef std::function<void(const char*, const uint8_t*, size_t)> DeliverFn;

struct PeerContext {
  bool connected = false;
  int pendingSubs = 0;
  std::map<std::string, DeliverFn> routes;
};

// ---------------------------------------------------------------------------
// libmosquitto transport adapter (outbound: publish/subscribe/unsubscribe).
// ---------------------------------------------------------------------------
class MosquittoAdapter : public ParleyConnectionAdapter {
 public:
  MosquittoAdapter(struct mosquitto* mosq, PeerContext* ctx) : mosq_(mosq), ctx_(ctx) {}

  void publish(const char* topic, const uint8_t* payload, size_t length) override {
    mosquitto_publish(mosq_, nullptr, topic, static_cast<int>(length), payload, /*qos=*/0, /*retain=*/false);
  }

  void subscribe(const char* topic) override {
    int mid = 0;
    if (mosquitto_subscribe(mosq_, &mid, topic, /*qos=*/0) == MOSQ_ERR_SUCCESS) {
      ctx_->pendingSubs++;
    }
  }

  void unsubscribe(const char* topic) override { mosquitto_unsubscribe(mosq_, nullptr, topic); }

 private:
  struct mosquitto* mosq_;
  PeerContext* ctx_;
};

// ---------------------------------------------------------------------------
// harness handler — the five fixed command behaviours.
// ---------------------------------------------------------------------------
enum HandlerKind { KEcho, KSlow, KErr, KCancelme, KBusy1 };

static HandlerKind kindOf(const std::string& cmd) {
  if (cmd == "echo") return KEcho;
  if (cmd == "slow") return KSlow;
  if (cmd == "err") return KErr;
  if (cmd == "cancelme") return KCancelme;
  return KBusy1;
}

static RawPayload makeProgress(int i) {
  RawPayload r;
  JsonDocument doc;
  doc["i"] = i;
  r.len = ArduinoJson::serializeJson(doc, r.bytes, sizeof(r.bytes));
  r.present = true;
  return r;
}

static RawPayload makeDone() {
  RawPayload r;
  JsonDocument doc;
  doc["done"] = true;
  r.len = ArduinoJson::serializeJson(doc, r.bytes, sizeof(r.bytes));
  r.present = true;
  return r;
}

static int readInt(const RawPayload& req, const char* key, int def) {
  if (!req.present) return def;
  JsonDocument doc;
  if (ArduinoJson::deserializeJson(doc, req.bytes, req.len) != ArduinoJson::DeserializationError::Ok) return def;
  return doc[key] | def;
}

static void readStr(const RawPayload& req, const char* key, char* out, size_t cap, const char* def) {
  const char* s = def;
  JsonDocument doc;
  if (req.present && ArduinoJson::deserializeJson(doc, req.bytes, req.len) == ArduinoJson::DeserializationError::Ok) {
    const char* v = doc[key].as<const char*>();
    if (v != nullptr) s = v;
  }
  size_t i = 0;
  for (; s[i] != '\0' && i + 1 < cap; ++i) out[i] = s[i];
  out[i] = '\0';
}

// Non-templated interface so handlers of different MaxCommands live in one container.
struct IPeerHandler {
  virtual ~IPeerHandler() {}
  virtual void doTick(uint32_t now) = 0;
  virtual void doPump(uint32_t now) = 0;
  virtual void doDeliver(const char* topic, const uint8_t* payload, size_t len) = 0;
};

template <int MaxCommands>
class HarnessHandler : public parley::ParleyCommandHandler<RawPayload, RawPayload, RawPayload, RawCode, MaxCommands, kMaxPayload>,
                       public IPeerHandler {
 public:
  typedef parley::ParleyCommandHandler<RawPayload, RawPayload, RawPayload, RawCode, MaxCommands, kMaxPayload> Base;
  typedef ParleyExecution<RawPayload, RawPayload, RawPayload, RawCode> Exec;

  HarnessHandler(ParleyConnectionAdapter& a, const ParleyTopics& t, const ParleyConfig& c, HandlerKind kind)
      : Base(a, t, c), kind_(kind) {}

  void doTick(uint32_t now) override { this->tick(now); }
  void doDeliver(const char* topic, const uint8_t* payload, size_t len) override {
    this->deliver(topic, payload, len);
  }

  void doPump(uint32_t now) override {
    if (exec_ == nullptr) return;
    if (kind_ == KSlow) {
      if (now - lastProgress_ >= static_cast<uint32_t>(everyMs_)) {
        exec_->reportProgress(makeProgress(progressCount_++));
        lastProgress_ = now;
      }
      if (now - start_ >= static_cast<uint32_t>(durationMs_)) {
        exec_->succeed(makeDone());
        exec_ = nullptr;
      }
    } else {  // cancelme / busy1
      if (exec_->isCancelRequested()) {
        exec_->cancel();
        exec_ = nullptr;
        return;
      }
      if (now - lastProgress_ >= static_cast<uint32_t>(everyMs_)) {
        exec_->reportProgress(makeProgress(progressCount_++));
        lastProgress_ = now;
      }
    }
  }

 protected:
  void handleCommand(Exec& execution) override {
    const RawPayload& req = execution.request();
    if (kind_ == KEcho) {
      execution.succeed(req);
      return;
    }
    if (kind_ == KErr) {
      RawCode rc;
      readStr(req, "code", rc.code, sizeof(rc.code), "error");
      execution.fail(rc);
      return;
    }
    // slow / cancelme / busy1: park and drive from doPump().
    everyMs_ = readInt(req, "progressEveryMs", 200);
    if (everyMs_ < 1) everyMs_ = 1;
    durationMs_ = (kind_ == KSlow) ? readInt(req, "durationMs", 1000) : 0;
    exec_ = &execution;
    start_ = nowMs();
    lastProgress_ = start_;
    execution.reportProgress(makeProgress(0));
    progressCount_ = 1;
  }

 private:
  HandlerKind kind_;
  Exec* exec_ = nullptr;
  int everyMs_ = 200;
  int durationMs_ = 0;
  int progressCount_ = 0;
  uint32_t start_ = 0;
  uint32_t lastProgress_ = 0;
};

// ---------------------------------------------------------------------------
// commander step interpreter (return-value error propagation; no exceptions).
// ---------------------------------------------------------------------------
struct CallRec {
  std::string command;
  ParleyCallHandle handle;
  int progress = 0;
  bool done = false;
  ParleyResult<RawPayload, RawCode> result;
};

static std::vector<PeerCommander*> g_commanders;  // pumped by pumpUntil
static struct mosquitto* g_mosq = nullptr;

static void onCommanderProgress(void* ud, const RawPayload&) { static_cast<CallRec*>(ud)->progress++; }
static void onCommanderComplete(void* ud, const ParleyResult<RawPayload, RawCode>& r) {
  CallRec* rec = static_cast<CallRec*>(ud);
  rec->result = r;
  rec->done = true;
}

static bool pumpUntil(const std::function<bool()>& pred, uint32_t timeoutMs) {
  uint32_t start = nowMs();
  for (;;) {
    mosquitto_loop(g_mosq, 5, 1);
    uint32_t now = nowMs();
    for (size_t i = 0; i < g_commanders.size(); ++i) g_commanders[i]->tick(now);
    if (pred()) return true;
    if (nowMs() - start >= timeoutMs) return false;
  }
}

static RawPayload rawFromVariant(JsonVariantConst v) {
  RawPayload r;
  if (v.isNull()) return r;
  r.len = ArduinoJson::serializeJson(v, r.bytes, sizeof(r.bytes));
  r.present = true;
  return r;
}

static std::string checkExpect(const std::string& name, JsonObjectConst expect, const CallRec& rec) {
  bool expOk = expect["ok"].as<bool>();
  const ParleyResult<RawPayload, RawCode>& res = rec.result;
  if (expOk) {
    if (!res.isSuccess()) {
      return name + ": expected success, got error '" + (res.isError() ? res.error().code() : "?") + "'";
    }
    if (objHasKey(expect, "p")) {
      if (!res.value().present) return name + ": expected a result payload but got none";
      JsonDocument actual;
      ArduinoJson::deserializeJson(actual, res.value().bytes, res.value().len);
      if (!peerJsonEqual(actual.as<JsonVariantConst>(), expect["p"])) {
        return name + ": result payload mismatch (got " + std::string(res.value().bytes, res.value().len) + ")";
      }
    }
  } else {
    if (!res.isError()) return name + ": expected error but succeeded";
    if (objHasKey(expect, "e")) {
      const char* wantE = expect["e"].as<const char*>();
      if (strcmp(res.error().code(), wantE) != 0) {
        return name + ": error code mismatch. expected '" + wantE + "', got '" + res.error().code() + "'";
      }
    }
  }
  if (objHasKey(expect, "minProgress")) {
    int minP = expect["minProgress"].as<int>();
    if (rec.progress < minP) {
      std::ostringstream os;
      os << name << ": minProgress not met. expected >= " << minP << ", got " << rec.progress;
      return os.str();
    }
  }
  return "";
}

static CallRec* startCall(JsonObjectConst call, std::map<std::string, PeerCommander*>& commanders,
                          std::deque<CallRec>& recs, std::string& err) {
  std::string command = call["command"].as<const char*>();
  std::map<std::string, PeerCommander*>::iterator it = commanders.find(command);
  if (it == commanders.end()) {
    err = "unknown command '" + command + "'";
    return nullptr;
  }
  recs.emplace_back();
  CallRec* rec = &recs.back();
  rec->command = command;
  RawPayload p = rawFromVariant(call["p"]);
  rec->handle = it->second->execute(p, ParleyCallOptions(), &onCommanderProgress, &onCommanderComplete, rec);
  return rec;
}

static std::string runScenario(JsonObjectConst scenario, std::map<std::string, PeerCommander*>& commanders,
                               uint32_t barrierMs, uint32_t callMs) {
  std::string name = scenario["name"].as<const char*>();
  std::deque<CallRec> recs;
  std::map<std::string, CallRec*> named;

  for (JsonVariantConst stepV : scenario["steps"].as<JsonArrayConst>()) {
    JsonObjectConst step = stepV.as<JsonObjectConst>();

    if (!step["call"].isNull()) {
      JsonObjectConst call = step["call"].as<JsonObjectConst>();
      std::string err;
      CallRec* rec = startCall(call, commanders, recs, err);
      if (rec == nullptr) return err;
      CallRec* r = rec;
      if (!pumpUntil([r]() { return r->done; }, callMs)) {
        return name + ": call '" + rec->command + "' — no terminal outcome within cap";
      }
      std::string e = checkExpect(name, call["expect"].as<JsonObjectConst>(), *rec);
      if (!e.empty()) return e;
    } else if (!step["callAsync"].isNull()) {
      JsonObjectConst call = step["callAsync"].as<JsonObjectConst>();
      std::string err;
      CallRec* rec = startCall(call, commanders, recs, err);
      if (rec == nullptr) return err;
      named[std::string(call["name"].as<const char*>())] = rec;
    } else if (!step["awaitProgress"].isNull()) {
      std::string target = step["awaitProgress"]["name"].as<const char*>();
      std::map<std::string, CallRec*>::iterator it = named.find(target);
      if (it == named.end()) return name + ": awaitProgress of unknown call '" + target + "'";
      CallRec* r = it->second;
      if (!pumpUntil([r]() { return r->progress >= 1 || r->done; }, barrierMs)) {
        return name + ": awaitProgress '" + target + "' — no progress within cap";
      }
      if (r->progress < 1 && r->done) {
        return name + ": awaitProgress '" + target + "' — terminal outcome before any progress";
      }
    } else if (!step["cancel"].isNull()) {
      std::string target = step["cancel"]["name"].as<const char*>();
      std::map<std::string, CallRec*>::iterator it = named.find(target);
      if (it == named.end()) return name + ": cancel of unknown call '" + target + "'";
      commanders[it->second->command]->cancel(it->second->handle);
    } else if (!step["await"].isNull()) {
      JsonObjectConst awaitStep = step["await"].as<JsonObjectConst>();
      std::string target = awaitStep["name"].as<const char*>();
      std::map<std::string, CallRec*>::iterator it = named.find(target);
      if (it == named.end()) return name + ": await of unknown call '" + target + "'";
      CallRec* r = it->second;
      if (!pumpUntil([r]() { return r->done; }, callMs)) {
        return name + ": await '" + target + "' — no terminal outcome within cap";
      }
      std::string e = checkExpect(name, awaitStep["expect"].as<JsonObjectConst>(), *r);
      if (!e.empty()) return e;
    } else {
      return name + ": unrecognized step";
    }
  }
  return "";
}

// ---------------------------------------------------------------------------
// config / helpers
// ---------------------------------------------------------------------------
static ParleyConfig buildConfig(JsonObjectConst c) {
  ParleyConfig cfg;
  cfg.requestRetryInterval = c["requestRetryInterval"] | cfg.requestRetryInterval;
  cfg.noAckTimeout = c["noAckTimeout"] | cfg.noAckTimeout;
  cfg.staleTimeout = c["staleTimeout"] | cfg.staleTimeout;
  cfg.executionTimeout = c["executionTimeout"] | cfg.executionTimeout;
  cfg.cancelRetryInterval = c["cancelRetryInterval"] | cfg.cancelRetryInterval;
  cfg.heartbeatInterval = c["heartbeatInterval"] | cfg.heartbeatInterval;
  cfg.resultRetryInterval = c["resultRetryInterval"] | cfg.resultRetryInterval;
  cfg.resultRetryTimeout = c["resultRetryTimeout"] | cfg.resultRetryTimeout;
  cfg.tombstoneTtl = c["tombstoneTtl"] | cfg.tombstoneTtl;
  cfg.maxConcurrentExecutions = c["maxConcurrentExecutions"] | cfg.maxConcurrentExecutions;
  return cfg;
}

struct TopicStore {
  std::deque<std::string> strings;
  ParleyTopics make(const std::string& prefix, const std::string& cmd) {
    strings.push_back(prefix + "/" + cmd + "/req");
    const char* req = strings.back().c_str();
    strings.push_back(prefix + "/" + cmd + "/hb");
    const char* hb = strings.back().c_str();
    strings.push_back(prefix + "/" + cmd + "/prg");
    const char* prg = strings.back().c_str();
    strings.push_back(prefix + "/" + cmd + "/res");
    const char* res = strings.back().c_str();
    return ParleyTopics(req, hb, prg, res);
  }
};

static std::map<std::string, std::string> parseArgs(int argc, char** argv) {
  std::map<std::string, std::string> out;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--", 0) == 0 && i + 1 < argc) {
      out[a.substr(2)] = argv[i + 1];
      ++i;
    }
  }
  return out;
}

static void parseBroker(const std::string& broker, std::string& host, int& port) {
  std::string s = broker;
  size_t scheme = s.find("://");
  if (scheme != std::string::npos) s = s.substr(scheme + 3);
  size_t colon = s.rfind(':');
  if (colon == std::string::npos) {
    host = s;
    port = 1883;
  } else {
    host = s.substr(0, colon);
    port = std::atoi(s.substr(colon + 1).c_str());
  }
}

// ---------------------------------------------------------------------------
// mosquitto callbacks
// ---------------------------------------------------------------------------
static void onConnectCb(struct mosquitto*, void* obj, int) {
  static_cast<PeerContext*>(obj)->connected = true;
}

static void onSubscribeCb(struct mosquitto*, void* obj, int, int, const int*) {
  PeerContext* ctx = static_cast<PeerContext*>(obj);
  if (ctx->pendingSubs > 0) ctx->pendingSubs--;
}

static void onMessageCb(struct mosquitto*, void* obj, const struct mosquitto_message* msg) {
  PeerContext* ctx = static_cast<PeerContext*>(obj);
  std::map<std::string, DeliverFn>::iterator it = ctx->routes.find(msg->topic);
  if (it != ctx->routes.end()) {
    it->second(msg->topic, static_cast<const uint8_t*>(msg->payload), static_cast<size_t>(msg->payloadlen));
  }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  std::map<std::string, std::string> opt = parseArgs(argc, argv);
  std::string role = opt.count("role") ? opt["role"] : "";
  std::string prefix = opt.count("prefix") ? opt["prefix"] : "";
  std::string scenariosPath = opt.count("scenarios") ? opt["scenarios"] : "scenarios.json";
  std::string suiteKey = (opt.count("suite") && opt["suite"] == "selfchecks") ? "selfChecks" : "scenarios";
  std::string handlerLang = opt.count("handler-lang") ? opt["handler-lang"] : "-";
  std::string host;
  int port = 1883;
  parseBroker(opt.count("broker") ? opt["broker"] : "localhost:1883", host, port);

  std::ifstream in(scenariosPath.c_str(), std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string text = ss.str();
  JsonDocument root;
  if (ArduinoJson::deserializeJson(root, text) != ArduinoJson::DeserializationError::Ok) {
    fprintf(stderr, "failed to parse scenarios file: %s\n", scenariosPath.c_str());
    return 2;
  }
  JsonObjectConst cfgObj = root["config"].as<JsonObjectConst>();
  ParleyConfig config = buildConfig(cfgObj);
  uint32_t barrierMs = root["timeouts"]["awaitProgressMs"] | 6000;
  uint32_t callMs = root["timeouts"]["callMs"] | 15000;

  static const char* kCommands[] = {"echo", "slow", "err", "cancelme", "busy1"};

  mosquitto_lib_init();
  PeerContext ctx;
  struct mosquitto* mosq = mosquitto_new(nullptr, /*clean_session=*/true, &ctx);
  g_mosq = mosq;
  mosquitto_connect_callback_set(mosq, onConnectCb);
  mosquitto_subscribe_callback_set(mosq, onSubscribeCb);
  mosquitto_message_callback_set(mosq, onMessageCb);

  if (mosquitto_connect(mosq, host.c_str(), port, /*keepalive=*/60) != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "mosquitto_connect failed to %s:%d\n", host.c_str(), port);
    return 2;
  }

  // Pump the CONNACK.
  {
    uint32_t start = nowMs();
    while (!ctx.connected && nowMs() - start < 10000) mosquitto_loop(mosq, 20, 1);
    if (!ctx.connected) {
      fprintf(stderr, "mqtt connection did not establish\n");
      return 2;
    }
  }

  MosquittoAdapter adapter(mosq, &ctx);
  TopicStore topics;

  if (role == "handler") {
    std::vector<IPeerHandler*> handlers;
    for (size_t i = 0; i < 5; ++i) {
      std::string cmd = kCommands[i];
      ParleyTopics t = topics.make(prefix, cmd);
      HandlerKind kind = kindOf(cmd);
      IPeerHandler* h;
      if (cmd == "busy1") {
        h = new HarnessHandler<1>(adapter, t, config, kind);
      } else {
        h = new HarnessHandler<8>(adapter, t, config, kind);
      }
      handlers.push_back(h);
      ctx.routes[prefix + "/" + cmd + "/req"] = [h](const char* tp, const uint8_t* p, size_t l) {
        h->doDeliver(tp, p, l);
      };
    }

    // Wait for all SUBACKs (subscriptions land at the broker) before signalling READY.
    {
      uint32_t start = nowMs();
      while (ctx.pendingSubs > 0 && nowMs() - start < 10000) mosquitto_loop(mosq, 20, 1);
    }
    printf("PARLEY-PEER-READY\n");
    fflush(stdout);

    for (;;) {
      mosquitto_loop(mosq, 50, 1);
      uint32_t now = nowMs();
      for (size_t i = 0; i < handlers.size(); ++i) {
        handlers[i]->doTick(now);
        handlers[i]->doPump(now);
      }
    }
    // not reached
  }

  // commander role
  std::map<std::string, PeerCommander*> commanders;
  for (size_t i = 0; i < 5; ++i) {
    std::string cmd = kCommands[i];
    ParleyTopics t = topics.make(prefix, cmd);
    PeerCommander* c = new PeerCommander(adapter, t, config);
    commanders[cmd] = c;
    g_commanders.push_back(c);
    ctx.routes[prefix + "/" + cmd + "/hb"] = [c](const char* tp, const uint8_t* p, size_t l) { c->deliver(tp, p, l); };
    ctx.routes[prefix + "/" + cmd + "/prg"] = [c](const char* tp, const uint8_t* p, size_t l) { c->deliver(tp, p, l); };
    ctx.routes[prefix + "/" + cmd + "/res"] = [c](const char* tp, const uint8_t* p, size_t l) { c->deliver(tp, p, l); };
  }

  // Wait for all SUBACKs before running the suite.
  {
    uint32_t start = nowMs();
    while (ctx.pendingSubs > 0 && nowMs() - start < 10000) mosquitto_loop(mosq, 20, 1);
  }

  JsonDocument report;
  report["commander"] = kSelfLang;
  report["handler"] = handlerLang;
  report["prefix"] = prefix;
  report["suite"] = suiteKey;
  JsonArray results = report["results"].to<JsonArray>();
  bool allPass = true;

  for (JsonVariantConst scenarioV : root[suiteKey.c_str()].as<JsonArrayConst>()) {
    JsonObjectConst scenario = scenarioV.as<JsonObjectConst>();
    std::string sname = scenario["name"].as<const char*>();
    std::string err = runScenario(scenario, commanders, barrierMs, callMs);
    bool pass = err.empty();
    if (!pass) allPass = false;
    JsonObject o = results.add<JsonObject>();
    o["scenario"] = sname;
    o["pass"] = pass;
    o["detail"] = err;
  }
  report["pass"] = allPass;

  std::string out;
  ArduinoJson::serializeJsonPretty(report, out);
  printf("%s\n", out.c_str());
  if (opt.count("report")) {
    std::ofstream f(opt["report"].c_str(), std::ios::binary);
    f << out;
  }

  mosquitto_disconnect(mosq);
  mosquitto_destroy(mosq);
  mosquitto_lib_cleanup();
  return allPass ? 0 : 1;
}
