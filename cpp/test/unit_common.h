#ifndef PARLEY_TEST_UNIT_COMMON_H
#define PARLEY_TEST_UNIT_COMMON_H

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "Parley.h"
#include "doctest.h"
#include "fake_adapter.h"
#include "json_compare.h"
#include "raw_json.h"

// Shared helpers for the unit suite. Payload types: `int` for the simple cases (default codec) and
// JsonPayload/RawCode (from raw_json.h) where arbitrary JSON or a wire error code is needed.

inline parley::ParleyTopics unitTopics() {
  return parley::ParleyTopics("request", "heartbeat", "progress", "result");
}

// --- wire message builders (ids are §4.2-safe, no escaping needed) ---
inline std::string msgReq(const char* id, const std::string& p = "{}") {
  return std::string("{\"v\":1,\"t\":\"req\",\"id\":\"") + id + "\",\"p\":" + p + "}";
}
inline std::string msgHb(const char* id) {
  return std::string("{\"v\":1,\"t\":\"hb\",\"id\":\"") + id + "\"}";
}
inline std::string msgPrg(const char* id, const std::string& p) {
  return std::string("{\"v\":1,\"t\":\"prg\",\"id\":\"") + id + "\",\"p\":" + p + "}";
}
inline std::string msgResOk(const char* id, const std::string& p) {
  return std::string("{\"v\":1,\"t\":\"res\",\"id\":\"") + id + "\",\"ok\":true,\"p\":" + p + "}";
}
inline std::string msgResErr(const char* id, const char* e) {
  return std::string("{\"v\":1,\"t\":\"res\",\"id\":\"") + id + "\",\"ok\":false,\"e\":\"" + e + "\"}";
}
inline std::string msgRak(const char* id) {
  return std::string("{\"v\":1,\"t\":\"rak\",\"id\":\"") + id + "\"}";
}
inline std::string msgCnl(const char* id) {
  return std::string("{\"v\":1,\"t\":\"cnl\",\"id\":\"") + id + "\"}";
}

template <class T>
void deliverStr(T& obj, const char* topic, const std::string& s) {
  obj.deliver(topic, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// --- outbound inspection ---
inline std::string publishType(const OutboundAction& a) {
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, a.payload.data(), a.payload.size()) !=
      ArduinoJson::DeserializationError::Ok) {
    return "";
  }
  const char* t = d["t"];
  return t ? std::string(t) : std::string();
}

inline int countPublishes(const FakeAdapter& a, const char* topic, const char* type) {
  int n = 0;
  for (size_t i = 0; i < a.outbound.size(); ++i) {
    if (a.outbound[i].isSubscribe || a.outbound[i].topic != topic) continue;
    if (publishType(a.outbound[i]) == type) ++n;
  }
  return n;
}

// Last publish on `topic`; returns nullptr if none.
inline const OutboundAction* lastPublish(const FakeAdapter& a, const char* topic) {
  const OutboundAction* found = nullptr;
  for (size_t i = 0; i < a.outbound.size(); ++i) {
    if (!a.outbound[i].isSubscribe && a.outbound[i].topic == topic) found = &a.outbound[i];
  }
  return found;
}

// The `e` field of the last res publish on the result topic (empty if none / not an error res).
inline std::string lastResultErrorCode(const FakeAdapter& a) {
  const OutboundAction* p = lastPublish(a, "result");
  if (p == nullptr) return "";
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, p->payload.data(), p->payload.size()) !=
      ArduinoJson::DeserializationError::Ok) {
    return "";
  }
  const char* e = d["e"];
  return e ? std::string(e) : std::string();
}

// A handler subclass that stashes each dispatched execution so a test can drive it directly.
template <class Req, class Prog, class Res, class Err, int N, int P = 256>
class StashHandler : public parley::ParleyCommandHandler<Req, Prog, Res, Err, N, P> {
 public:
  typedef parley::ParleyCommandHandler<Req, Prog, Res, Err, N, P> Base;
  typedef parley::ParleyExecution<Req, Prog, Res, Err> Exec;

  StashHandler(parley::ParleyConnectionAdapter& a, const parley::ParleyTopics& t,
               const parley::ParleyConfig& c = parley::ParleyConfig())
      : Base(a, t, c) {}

  Exec* exec(const std::string& id) {
    typename std::map<std::string, Exec*>::iterator it = pending_.find(id);
    return it == pending_.end() ? nullptr : it->second;
  }
  int dispatchCount() const { return dispatchCount_; }

 protected:
  void handleCommand(Exec& e) override {
    pending_[e.commandId()] = &e;
    ++dispatchCount_;
  }

 private:
  std::map<std::string, Exec*> pending_;
  int dispatchCount_ = 0;
};

// A no-heap adapter (plain counters, no std::vector) for the zero-heap-in-the-core probe.
class SilentAdapter : public parley::ParleyConnectionAdapter {
 public:
  int publishes = 0;
  int subscribes = 0;
  bool failPublish = false;
  void publish(const char*, const uint8_t*, size_t) override {
    ++publishes;
  }
  void subscribe(const char*) override { ++subscribes; }
  void unsubscribe(const char*) override {}
};

// A no-heap id generator producing "u0","u1",... — unique across a run, for the bounded-state /
// zero-heap tests (the scripted generator uses std::vector and would allocate).
class CountingIdGenerator : public parley::ParleyIdGenerator {
 public:
  void next(char* out, size_t cap) override {
    int n = snprintf(out, cap, "u%lu", static_cast<unsigned long>(counter_++));
    (void)n;
  }

 private:
  unsigned long counter_ = 0;
};

#endif  // PARLEY_TEST_UNIT_COMMON_H
