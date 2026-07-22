#include "unit_common.h"

using parley::ParleyConfig;

namespace {

typedef StashHandler<int, int, int, RawCode, 2> IntHandler2;
typedef StashHandler<int, int, int, RawCode, 1> IntHandler1;

}  // namespace

// --- H7 arm 1: EXECUTING → poll flag, handler produces the outcome ---
TEST_CASE("H7 executing arm: cnl sets the poll flag; handler completes as cancelled") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();

  deliverStr(h, "request", msgReq("A", "1"));
  CHECK(countPublishes(adapter, "heartbeat", "hb") == 1);  // H2 accept-ack
  IntHandler1::Exec* e = h.exec("A");
  REQUIRE(e != nullptr);
  CHECK_FALSE(e->isCancelRequested());

  deliverStr(h, "request", msgCnl("A"));
  CHECK(e->isCancelRequested());  // H7 EXECUTING: poll flag set, no res yet

  e->cancel();  // handler observes cancellation and aborts
  CHECK(lastResultErrorCode(adapter) == "$cancelled");
  CHECK(h.commandCount() == 1);  // RESULT-CACHED until rak
}

// --- H7 arm 2: QUEUED → dequeue + synthesized $cancelled, cached & retried ---
TEST_CASE("H7 queued arm: cnl dequeues and produces a cached, retried $cancelled") {
  FakeAdapter adapter;
  ParleyConfig cfg;
  cfg.heartbeatInterval = 10000;
  cfg.resultRetryInterval = 2000;
  cfg.maxConcurrentExecutions = 1;             // serial → B stays queued behind A
  IntHandler2 h(adapter, unitTopics(), cfg);   // pool of 2: both admitted, B queued
  adapter.clear();

  deliverStr(h, "request", msgReq("A", "1"));  // admitted + executing
  deliverStr(h, "request", msgReq("B", "1"));  // admitted + queued
  CHECK(h.queueCount() == 1);
  adapter.clear();

  deliverStr(h, "request", msgCnl("B"));
  CHECK(lastResultErrorCode(adapter) == "$cancelled");
  CHECK(h.queueCount() == 0);
  int firstRes = countPublishes(adapter, "result", "res");

  h.tick(2000);  // H5: the $cancelled result is retried
  CHECK(countPublishes(adapter, "result", "res") == firstRes + 1);
}

// --- H7 arm 3: RESULT-CACHED → re-publish the cached result (cancel lost the race) ---
TEST_CASE("H7 result-cached arm: cnl re-publishes the cached result") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();

  deliverStr(h, "request", msgReq("A", "1"));
  h.exec("A")->succeed(7);
  CHECK(countPublishes(adapter, "result", "res") == 1);
  adapter.clear();

  deliverStr(h, "request", msgCnl("A"));  // cancel lost the race
  CHECK(countPublishes(adapter, "result", "res") == 1);  // cached res re-published
}

// --- H7 arm 4: unknown/tombstoned → stateless $cancelled ---
TEST_CASE("H7 unknown arm: cnl for an unknown id → stateless $cancelled, no state") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();

  deliverStr(h, "request", msgCnl("ghost"));
  CHECK(lastResultErrorCode(adapter) == "$cancelled");
  CHECK(h.commandCount() == 0);       // stateless — no slot created
  CHECK(h.tombstoneCount() == 0);
}

// --- H1/H2/H8: admission, dedup-before-capacity, stateless $busy ---
TEST_CASE("admission: dedup precedes capacity; full pool answers stateless $busy") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());  // MaxCommands == 1: the pool IS the cap
  adapter.clear();

  deliverStr(h, "request", msgReq("A", "1"));  // admitted
  CHECK(countPublishes(adapter, "heartbeat", "hb") == 1);
  adapter.clear();

  deliverStr(h, "request", msgReq("B", "1"));  // full → stateless $busy
  CHECK(lastResultErrorCode(adapter) == "$busy");
  CHECK(h.commandCount() == 1);  // no state created for B
  adapter.clear();

  deliverStr(h, "request", msgReq("A", "1"));  // dup of admitted A → hb, NOT $busy (H1 before H8)
  CHECK(countPublishes(adapter, "heartbeat", "hb") == 1);
  CHECK(countPublishes(adapter, "result", "res") == 0);
}

// --- H1: duplicate of a RESULT-CACHED id re-publishes the cached res ---
TEST_CASE("dedup: duplicate req while result-cached re-publishes the cached result") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));
  h.exec("A")->succeed(1);
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));
  CHECK(countPublishes(adapter, "result", "res") == 1);
}

// --- H4: maxConcurrentExecutions ---
TEST_CASE("H4: maxConcurrentExecutions=2 dispatches two commands concurrently") {
  FakeAdapter adapter;
  ParleyConfig cfg;
  cfg.maxConcurrentExecutions = 2;
  IntHandler2 h(adapter, unitTopics(), cfg);
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));
  deliverStr(h, "request", msgReq("B", "1"));
  CHECK(h.dispatchCount() == 2);
  CHECK(h.executingCount() == 2);
}

// --- progress delivery (C8/H4) ---
TEST_CASE("handler emits progress on the progress topic") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));
  h.exec("A")->reportProgress(99);
  CHECK(countPublishes(adapter, "progress", "prg") == 1);
}

// --- publish-comes-last under a synchronous-loopback adapter (CRITICAL) ---
namespace {
struct RakLoopCtx {
  IntHandler1* handler;
};
void rakOnRes(void* ctx, const char* topic, const uint8_t* payload, size_t len) {
  if (strcmp(topic, "result") != 0) return;
  ArduinoJson::JsonDocument d;
  if (ArduinoJson::deserializeJson(d, payload, len) != ArduinoJson::DeserializationError::Ok) return;
  const char* id = d["id"];
  const char* t = d["t"];
  if (id == nullptr || t == nullptr || strcmp(t, "res") != 0) return;
  RakLoopCtx* c = static_cast<RakLoopCtx*>(ctx);
  std::string rak = msgRak(id);
  c->handler->deliver("request", reinterpret_cast<const uint8_t*>(rak.data()), rak.size());
}
}  // namespace

TEST_CASE("sync-loopback: a rak delivered from inside the res publish tombstones cleanly (no orphan retry)") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  RakLoopCtx ctx{&h};
  adapter.onPublish = &rakOnRes;
  adapter.onPublishCtx = &ctx;
  adapter.clear();

  deliverStr(h, "request", msgReq("A", "1"));
  h.exec("A")->succeed(5);  // publishes res → hook delivers rak re-entrantly → moveToTombstone

  CHECK(countPublishes(adapter, "result", "res") == 1);  // exactly one res, no orphan re-publish
  CHECK(h.commandCount() == 0);
  CHECK(h.tombstoneCount() == 1);
  adapter.onPublish = nullptr;

  h.tick(100000);  // no self-rearming result retry survived
  CHECK(countPublishes(adapter, "result", "res") == 1);
  CHECK(h.tombstoneCount() == 0);  // tombstone TTL expired
}

// --- inbound oversize is dropped (F2) ---
TEST_CASE("F2: an inbound req larger than MaxPayload is silently dropped") {
  FakeAdapter adapter;
  StashHandler<JsonPayload, JsonPayload, JsonPayload, RawCode, 1, 64> h(adapter, unitTopics());
  adapter.clear();

  std::string big(200, 'x');
  deliverStr(h, "request", msgReq("A", "\"" + big + "\""));  // > 64 bytes → dropped
  CHECK(countPublishes(adapter, "heartbeat", "hb") == 0);
  CHECK(h.commandCount() == 0);

  deliverStr(h, "request", msgReq("B", "1"));  // fits → admitted
  CHECK(countPublishes(adapter, "heartbeat", "hb") == 1);
}

// --- outbound oversize outcome aborts the command (handler fault) ---
TEST_CASE("outbound oversize: an over-MaxPayload result aborts (no res, slot freed, tombstoned)") {
  FakeAdapter adapter;
  StashHandler<JsonPayload, JsonPayload, JsonPayload, RawCode, 1, 64> h(adapter, unitTopics());
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));

  JsonPayload big;
  std::string s = "\"" + std::string(200, 'y') + "\"";
  memcpy(big.bytes, s.data(), s.size());
  big.len = s.size();
  big.present = true;

  h.exec("A")->succeed(big);  // serialized > 64 → encode failure ≡ handler fault → abort
  CHECK(countPublishes(adapter, "result", "res") == 0);
  CHECK(h.commandCount() == 0);
  CHECK(h.tombstoneCount() == 1);
}

// --- $-prefix rejection on a custom error is a handler fault → abort ---
TEST_CASE("custom error with a $-reserved code is rejected (abort, no wire res)") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));

  RawCode bad;
  strcpy(bad.code, "$sneaky");
  h.exec("A")->fail(bad);  // $-prefix reserved → abort
  CHECK(countPublishes(adapter, "result", "res") == 0);
  CHECK(h.commandCount() == 0);
  CHECK(h.tombstoneCount() == 1);
}

TEST_CASE("custom error with a valid code publishes an error res carrying the code") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));
  RawCode err;
  strcpy(err.code, "diskFull");
  h.exec("A")->fail(err);
  CHECK(lastResultErrorCode(adapter) == "diskFull");
}

// --- reconnect resubscription (§2) ---
TEST_CASE("reconnect: connected() re-subscribes the request topic") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();
  h.connected();
  int subs = 0;
  for (size_t i = 0; i < adapter.outbound.size(); ++i) {
    if (adapter.outbound[i].isSubscribe && adapter.outbound[i].topic == "request") ++subs;
  }
  CHECK(subs == 1);
}

// --- H6: a rak for an id not in RESULT-CACHED is ignored ---
TEST_CASE("H6: rak for an executing/unknown id is ignored") {
  FakeAdapter adapter;
  IntHandler1 h(adapter, unitTopics());
  adapter.clear();
  deliverStr(h, "request", msgReq("A", "1"));  // executing, not cached
  deliverStr(h, "request", msgRak("A"));        // ignored (not RESULT-CACHED)
  CHECK(h.commandCount() == 1);
  deliverStr(h, "request", msgRak("Z"));        // unknown id ignored
  CHECK(h.commandCount() == 1);
}
