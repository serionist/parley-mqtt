#include "unit_common.h"

#include "detail/time_util.h"

using parley::ParleyCallHandle;
using parley::ParleyCallOptions;
using parley::ParleyConfig;
using parley::ParleyResult;

namespace {

struct Cap {
  ParleyResult<int, RawCode> result;
  bool done;
  Cap() : done(false) {}
};
void onDone(void* ud, const ParleyResult<int, RawCode>& r) {
  Cap* c = static_cast<Cap*>(ud);
  c->result = r;
  c->done = true;
}

typedef parley::ParleyCommander<int, int, int, RawCode, 2, 256> Commander2;

}  // namespace

// --- happy path terminal delivery + rak ---
TEST_CASE("commander: hb then res → success outcome and a rak") {
  FakeAdapter adapter;
  Commander2 c(adapter, unitTopics());
  adapter.clear();
  Cap cap;
  c.execute(11, &onDone, &cap);
  const char* id = nullptr;
  {  // recover the generated id from the req publish
    const OutboundAction* req = lastPublish(adapter, "request");
    REQUIRE(req != nullptr);
    ArduinoJson::JsonDocument d;
    ArduinoJson::deserializeJson(d, req->payload.data(), req->payload.size());
    static std::string sid;
    sid = d["id"].as<const char*>();
    id = sid.c_str();
  }
  deliverStr(c, "heartbeat", msgHb(id));
  deliverStr(c, "result", msgResOk(id, "42"));
  CHECK(cap.done);
  CHECK(cap.result.isSuccess());
  CHECK(cap.result.value() == 42);
  CHECK(countPublishes(adapter, "request", "rak") == 1);
  CHECK(c.inflightCount() == 0);
  CHECK(c.tombstoneCount() == 1);
}

// --- F7: pool exhaustion → synchronous local failure (invalid handle, never a $ code) ---
TEST_CASE("F7: execute on a full in-flight pool returns an invalid handle") {
  FakeAdapter adapter;
  parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics());
  adapter.clear();
  Cap cap1, cap2;
  ParleyCallHandle h1 = c.execute(1, &onDone, &cap1);
  ParleyCallHandle h2 = c.execute(2, &onDone, &cap2);  // pool full
  CHECK(h1.valid());
  CHECK_FALSE(h2.valid());
  CHECK_FALSE(cap2.done);  // no outcome delivered; not a $ wire code
  CHECK(c.inflightCount() == 1);
}

// --- outbound oversize request → synchronous local failure ---
TEST_CASE("oversize request payload → invalid handle, no publish") {
  FakeAdapter adapter;
  parley::ParleyCommander<JsonPayload, int, int, RawCode, 1, 64> c(adapter, unitTopics());
  adapter.clear();
  JsonPayload big;
  std::string s = "\"" + std::string(200, 'z') + "\"";
  memcpy(big.bytes, s.data(), s.size());
  big.len = s.size();
  big.present = true;
  Cap cap;
  ParleyCallHandle h = c.execute(big, ParleyCallOptions(), nullptr, &onDone, &cap);
  CHECK_FALSE(h.valid());
  CHECK(adapter.publishCount() == 0);
  CHECK(c.inflightCount() == 0);
}

// --- $decodeError: a success payload that fails to decode raks and terminates $decodeError ---
TEST_CASE("C4: success res that fails to decode → rak + $decodeError") {
  FakeAdapter adapter;
  Commander2 c(adapter, unitTopics());
  adapter.clear();
  Cap cap;
  c.execute(0, &onDone, &cap);
  const OutboundAction* req = lastPublish(adapter, "request");
  ArduinoJson::JsonDocument d;
  ArduinoJson::deserializeJson(d, req->payload.data(), req->payload.size());
  std::string id = d["id"].as<const char*>();

  deliverStr(c, "result", msgResOk(id.c_str(), "\"not-an-int\""));  // Res=int, payload is a string
  CHECK(cap.done);
  CHECK(cap.result.isError());
  CHECK(std::string(cap.result.error().code()) == "$decodeError");
  CHECK(cap.result.error().isBuiltin());
  CHECK(countPublishes(adapter, "request", "rak") == 1);  // rak still sent (C4)
}

// --- error typing: custom code surfaces typed + raw; builtin surfaces classified ---
TEST_CASE("error typing: custom wire code surfaces as a typed custom error") {
  FakeAdapter adapter;
  Commander2 c(adapter, unitTopics());
  adapter.clear();
  Cap cap;
  c.execute(0, &onDone, &cap);
  const OutboundAction* req = lastPublish(adapter, "request");
  ArduinoJson::JsonDocument d;
  ArduinoJson::deserializeJson(d, req->payload.data(), req->payload.size());
  std::string id = d["id"].as<const char*>();

  deliverStr(c, "result", msgResErr(id.c_str(), "diskFull"));
  CHECK(cap.result.isError());
  CHECK_FALSE(cap.result.error().isBuiltin());
  CHECK(std::string(cap.result.error().code()) == "diskFull");
  CHECK(std::string(cap.result.error().custom().code) == "diskFull");
  CHECK(countPublishes(adapter, "request", "rak") == 1);
}

// --- C3: execution cap positive control vs kNoTimeout disable ---
TEST_CASE("C3: executionTimeout fires; kNoTimeout disables the cap (explicit skip)") {
  ParleyConfig cfg;
  cfg.noAckTimeout = 1000000;
  cfg.staleTimeout = 1000000;  // isolate the execution cap
  cfg.executionTimeout = 8000;

  SUBCASE("positive control: the cap fires") {
    FakeAdapter adapter;
    parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics(), cfg);
    adapter.clear();
    Cap cap;
    c.execute(0, &onDone, &cap);
    const OutboundAction* req = lastPublish(adapter, "request");
    ArduinoJson::JsonDocument d;
    ArduinoJson::deserializeJson(d, req->payload.data(), req->payload.size());
    std::string id = d["id"].as<const char*>();
    deliverStr(c, "heartbeat", msgHb(id.c_str()));  // → LIVE, execution cap armed
    c.tick(8000);
    CHECK(cap.done);
    CHECK(std::string(cap.result.error().code()) == "$executionTimeout");
  }

  SUBCASE("kNoTimeout disables the cap") {
    FakeAdapter adapter;
    parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics(), cfg);
    adapter.clear();
    Cap cap;
    ParleyCallOptions opts;
    opts.executionTimeout = parley::detail::kNoTimeout;  // disable
    c.execute(0, opts, nullptr, &onDone, &cap);
    const OutboundAction* req = lastPublish(adapter, "request");
    ArduinoJson::JsonDocument d;
    ArduinoJson::deserializeJson(d, req->payload.data(), req->payload.size());
    std::string id = d["id"].as<const char*>();
    deliverStr(c, "heartbeat", msgHb(id.c_str()));
    c.tick(500000);  // far past the (disabled) 8000ms cap, before the 1000000ms stale window
    CHECK_FALSE(cap.done);  // no execution-cap terminal
    CHECK(c.inflightCount() == 1);
  }
}

// --- adapter-publish-failure self-heal: dropped req publishes don't stall the retry cadence ---
TEST_CASE("adapter drop: request retries self-heal after dropped publishes") {
  ParleyConfig cfg;
  cfg.requestRetryInterval = 2000;
  cfg.noAckTimeout = 1000000;
  FakeAdapter adapter;
  parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics(), cfg);
  adapter.clear();
  adapter.dropPublishes = 2;  // drop the initial req and the first retry
  Cap cap;
  c.execute(0, &onDone, &cap);       // dropped
  CHECK(adapter.publishCount() == 0);
  c.tick(2000);                      // retry — dropped
  CHECK(adapter.publishCount() == 0);
  c.tick(4000);                      // retry — delivered (cadence survived)
  CHECK(countPublishes(adapter, "request", "req") == 1);
  c.tick(6000);
  CHECK(countPublishes(adapter, "request", "req") == 2);
}

// --- publish-comes-last: a $cancelled res delivered from inside the cnl publish tears down cleanly ---
namespace {
struct CnlLoopCtx {
  parley::ParleyCommander<int, int, int, RawCode, 1, 256>* commander;
};
void resOnCnl(void* ctx, const char* topic, const uint8_t* payload, size_t len) {
  if (strcmp(topic, "request") != 0) return;
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, payload, len) != ArduinoJson::DeserializationError::Ok) return;
  const char* t = d["t"];
  const char* id = d["id"];
  if (t == nullptr || id == nullptr || strcmp(t, "cnl") != 0) return;
  CnlLoopCtx* c = static_cast<CnlLoopCtx*>(ctx);
  std::string res = msgResErr(id, "$cancelled");
  c->commander->deliver("result", reinterpret_cast<const uint8_t*>(res.data()), res.size());
}
}  // namespace

TEST_CASE("sync-loopback: $cancelled res from inside the cnl publish disposes the cancel-retry timer") {
  FakeAdapter adapter;
  parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics());
  CnlLoopCtx ctx{&c};
  adapter.clear();
  Cap cap;
  ParleyCallHandle h = c.execute(0, &onDone, &cap);
  const OutboundAction* req = lastPublish(adapter, "request");
  ArduinoJson::JsonDocument d;
  ArduinoJson::deserializeJson(d, req->payload.data(), req->payload.size());
  std::string id = d["id"].as<const char*>();
  deliverStr(c, "heartbeat", msgHb(id.c_str()));  // LIVE
  adapter.clear();

  adapter.onPublish = &resOnCnl;
  adapter.onPublishCtx = &ctx;
  c.cancel(h);  // publishes cnl → hook delivers $cancelled res re-entrantly → teardown

  CHECK(cap.done);
  CHECK(std::string(cap.result.error().code()) == "$cancelled");
  CHECK(countPublishes(adapter, "request", "cnl") == 1);
  CHECK(c.inflightCount() == 0);
  adapter.onPublish = nullptr;

  c.tick(100000);  // no orphaned self-rearming cnl timer
  CHECK(countPublishes(adapter, "request", "cnl") == 1);
}

// --- publish-comes-last (success path): a res bounced back during the rak publish must not double-complete ---
namespace {
struct SuccessLoopCtx {
  parley::ParleyCommander<int, int, int, RawCode, 1, 256>* commander;
  std::string id;
};
void resOnRak(void* ctx, const char* topic, const uint8_t* payload, size_t len) {
  if (strcmp(topic, "request") != 0) return;
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, payload, len) != ArduinoJson::DeserializationError::Ok) return;
  const char* t = d["t"];
  if (t == nullptr || strcmp(t, "rak") != 0) return;
  SuccessLoopCtx* c = static_cast<SuccessLoopCtx*>(ctx);
  std::string res = msgResOk(c->id.c_str(), "1");  // duplicate result bounced back
  c->commander->deliver("result", reinterpret_cast<const uint8_t*>(res.data()), res.size());
}
int g_completeCalls = 0;
void countingComplete(void*, const ParleyResult<int, RawCode>&) { ++g_completeCalls; }
}  // namespace

TEST_CASE("sync-loopback: a duplicate res during the rak publish re-raks but never double-completes") {
  FakeAdapter adapter;
  parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics());
  SuccessLoopCtx ctx{&c, ""};
  adapter.clear();
  g_completeCalls = 0;
  c.execute(0, ParleyCallOptions(), nullptr, &countingComplete, nullptr);
  const OutboundAction* req = lastPublish(adapter, "request");
  ArduinoJson::JsonDocument d;
  ArduinoJson::deserializeJson(d, req->payload.data(), req->payload.size());
  ctx.id = d["id"].as<const char*>();

  adapter.onPublish = &resOnRak;
  adapter.onPublishCtx = &ctx;
  std::string res = msgResOk(ctx.id.c_str(), "1");
  c.deliver("result", reinterpret_cast<const uint8_t*>(res.data()), res.size());  // terminal success

  CHECK(g_completeCalls == 1);  // completed exactly once despite the loopback
  CHECK(c.inflightCount() == 0);
  CHECK(c.tombstoneCount() == 1);
  CHECK(countPublishes(adapter, "request", "rak") == 2);  // original rak + re-rak from the tombstone
  adapter.onPublish = nullptr;
}

// --- wraparound: a request-retry timer armed near UINT32_MAX fires correctly after the rollover ---
TEST_CASE("wraparound: request retry fires across the uint32 millis rollover") {
  ParleyConfig cfg;
  cfg.requestRetryInterval = 2000;
  cfg.noAckTimeout = 1000000;  // won't fire during the window
  FakeAdapter adapter;
  parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics(), cfg);
  adapter.clear();

  uint32_t start = 0xFFFFFFFFu - 500;  // 500 ms before rollover
  c.tick(start);
  Cap cap;
  c.execute(0, &onDone, &cap);  // req at `start`; retry due = start + 2000 → wraps to ~1499
  CHECK(countPublishes(adapter, "request", "req") == 1);

  c.tick(1499);  // after the rollover; the retry is due
  CHECK(countPublishes(adapter, "request", "req") == 2);
  c.tick(3499);
  CHECK(countPublishes(adapter, "request", "req") == 3);
}

// --- reconnect: connected() re-subscribes all three commander topics ---
TEST_CASE("reconnect: connected() re-subscribes heartbeat, progress, result") {
  FakeAdapter adapter;
  Commander2 c(adapter, unitTopics());
  adapter.clear();
  c.connected();
  int subs = 0;
  for (size_t i = 0; i < adapter.outbound.size(); ++i) {
    if (adapter.outbound[i].isSubscribe) ++subs;
  }
  CHECK(subs == 3);
}

// --- C6: foreign-id traffic is ignored ---
TEST_CASE("C6: inbound for a foreign id is ignored") {
  FakeAdapter adapter;
  Commander2 c(adapter, unitTopics());
  adapter.clear();
  Cap cap;
  c.execute(0, &onDone, &cap);
  deliverStr(c, "result", msgResOk("someoneElse", "1"));  // not our id
  CHECK_FALSE(cap.done);
  CHECK(countPublishes(adapter, "request", "rak") == 0);
}

// --- CC1: reserveCall rejects an id colliding with an in-flight or tombstoned call (§4.2) ---
TEST_CASE("CC1: a generated id colliding with an in-flight call is a synchronous local failure") {
  FakeAdapter adapter;
  ScriptedIdGenerator idgen;
  idgen.enqueue("dup");
  idgen.enqueue("dup");  // a weak generator repeats the id
  parley::ParleyCommander<int, int, int, RawCode, 2, 256> c(adapter, unitTopics(), ParleyConfig(),
                                                            &idgen);
  adapter.clear();
  Cap cap1, cap2;
  ParleyCallHandle h1 = c.execute(1, &onDone, &cap1);
  ParleyCallHandle h2 = c.execute(2, &onDone, &cap2);  // "dup" collides in-flight → rejected
  CHECK(h1.valid());
  CHECK_FALSE(h2.valid());
  CHECK_FALSE(cap2.done);  // local failure — no $ wire code delivered
  CHECK(c.inflightCount() == 1);
  CHECK(countPublishes(adapter, "request", "req") == 1);  // only the first call published
}

TEST_CASE("CC1: a generated id colliding with a live tombstone is a synchronous local failure") {
  FakeAdapter adapter;
  ScriptedIdGenerator idgen;
  idgen.enqueue("solo");
  idgen.enqueue("solo");
  parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics(), ParleyConfig(),
                                                            &idgen);
  adapter.clear();
  Cap cap1;
  c.execute(1, &onDone, &cap1);
  deliverStr(c, "result", msgResOk("solo", "9"));  // complete call 1 → tombstone "solo"
  CHECK(c.tombstoneCount() == 1);
  adapter.clear();

  Cap cap2;
  ParleyCallHandle h2 = c.execute(2, &onDone, &cap2);  // "solo" still tombstoned → rejected
  CHECK_FALSE(h2.valid());
  CHECK_FALSE(cap2.done);
  CHECK(c.inflightCount() == 0);
  CHECK(adapter.publishCount() == 0);  // nothing published for the rejected call
}

// --- CC2: sendCall captures the generation BEFORE publish (instant-complete loopback + re-entrant
// execute). Without the capture, the returned handle would alias the slot's NEW occupant. ---
namespace {
struct CC2Ctx {
  parley::ParleyCommander<int, int, int, RawCode, 1, 256>* c;
  Cap* cap2;
  bool reentered;
  ParleyCallHandle h2;
};
void cc2Loop(void* ctx, const char* topic, const uint8_t* payload, size_t len) {
  CC2Ctx* x = static_cast<CC2Ctx*>(ctx);
  if (x->reentered || strcmp(topic, "request") != 0) return;
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, payload, len) != ArduinoJson::DeserializationError::Ok) return;
  const char* t = d["t"];
  const char* id = d["id"];
  if (t == nullptr || id == nullptr || strcmp(t, "req") != 0) return;
  x->reentered = true;
  // Instant-complete call 1 from inside its own req publish: frees + tombstones slot 0.
  std::string res = msgResOk(id, "1");
  x->c->deliver("result", reinterpret_cast<const uint8_t*>(res.data()), res.size());
  // Re-entrant execute for call 2 → reuses slot 0, bumping its generation.
  x->h2 = x->c->execute(2, &onDone, x->cap2);
}
}  // namespace

TEST_CASE("CC2: the call handle captures the generation before publish (loopback + re-entrant execute)") {
  FakeAdapter adapter;
  parley::ParleyCommander<int, int, int, RawCode, 1, 256> c(adapter, unitTopics());
  Cap cap1, cap2;
  CC2Ctx ctx{&c, &cap2, false, ParleyCallHandle::invalid()};
  adapter.clear();
  adapter.onPublish = &cc2Loop;
  adapter.onPublishCtx = &ctx;

  ParleyCallHandle h1 = c.execute(1, &onDone, &cap1);  // req1 → loopback completes it, then starts call 2
  adapter.onPublish = nullptr;

  CHECK(cap1.done);
  CHECK(cap1.result.isSuccess());  // call 1 completed via the loopback res
  CHECK(ctx.reentered);
  CHECK(ctx.h2.valid());
  // Both handles name slot 0, but MUST differ in generation — h1 captured its gen before the reuse.
  CHECK(h1.slot == ctx.h2.slot);
  CHECK(h1.gen != ctx.h2.gen);
  CHECK(c.inflightCount() == 1);  // only call 2 remains in flight

  // Cancelling the STALE h1 must not touch call 2 — the aliasing the capture prevents.
  c.cancel(h1);
  CHECK(countPublishes(adapter, "request", "cnl") == 0);  // no cnl emitted for the stale handle
  CHECK_FALSE(cap2.done);                                  // call 2 untouched, still in flight
  CHECK(c.inflightCount() == 1);
}
