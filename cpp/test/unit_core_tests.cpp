#include <stdio.h>
#include <string.h>

#include "alloc_counter.h"
#include "detail/core_util.h"
#include "detail/fixed_arena.h"
#include "detail/time_util.h"
#include "detail/wire.h"
#include "json_compare.h"
#include "unit_common.h"

using namespace parley::detail;

// --- wraparound-safe time comparisons (the 49.7-day rollover) ---
TEST_CASE("time_util: signed-diff comparisons are correct across the uint32 rollover") {
  CHECK(timeReached(1000, 1000));
  CHECK(timeReached(1001, 1000));
  CHECK_FALSE(timeReached(999, 1000));

  uint32_t near = 0xFFFFFFFFu - 100;
  uint32_t after = near + 300;  // wraps past 0 to ~199
  CHECK(timeReached(after, near + 200));       // reached across the wrap
  CHECK_FALSE(timeReached(near, after));       // not yet reached
  CHECK(timeBefore(near, after));              // near precedes after across the wrap
  CHECK_FALSE(timeBefore(after, near));
}

// --- fixed arena: bounded, resettable, exhaustion returns nullptr ---
TEST_CASE("FixedArena: bump allocation, exhaustion, reset") {
  uint8_t buf[256];
  FixedArena arena(buf, sizeof(buf));
  void* a = arena.allocate(32);
  void* b = arena.allocate(32);
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  CHECK(a != b);
  CHECK(arena.allocate(100000) == nullptr);  // exhausted → nullptr (drives ArduinoJson NoMemory)
  arena.reset();
  CHECK(arena.allocate(32) != nullptr);      // reusable after reset
}

// --- tombstone store: membership, TTL expiry, oldest-first cap eviction ---
TEST_CASE("TombstoneStore: TTL expiry and oldest-first hard cap") {
  const int cap = 3;
  char ids[cap][parley::kIdCapacity];
  uint32_t due[cap];
  TombstoneStore store;
  store.init(ids, due, cap);

  store.add("a", 1000);
  store.add("b", 1000);
  store.add("c", 1000);
  CHECK(store.count() == 3);
  CHECK(store.contains("a"));

  store.add("d", 1000);  // over cap → evict oldest ("a")
  CHECK(store.count() == 3);
  CHECK_FALSE(store.contains("a"));
  CHECK(store.contains("d"));

  store.expire(999);  // nothing due yet
  CHECK(store.count() == 3);
  store.expire(1000);  // all TTLs reached
  CHECK(store.count() == 0);
}

TEST_CASE("TombstoneStore: refresh keeps position and updates TTL") {
  const int cap = 4;
  char ids[cap][parley::kIdCapacity];
  uint32_t due[cap];
  TombstoneStore store;
  store.init(ids, due, cap);
  store.add("x", 500);
  store.add("x", 900);  // refresh
  CHECK(store.count() == 1);
  store.expire(500);
  CHECK(store.count() == 1);  // still alive (refreshed to 900)
  store.expire(900);
  CHECK(store.count() == 0);
}

// --- id validation (§4.2) ---
TEST_CASE("wire::isValidId enforces §4.2") {
  CHECK(wire::isValidId("kD3fx9Qw"));
  CHECK(wire::isValidId("A"));
  CHECK(wire::isValidId("a-b_C9"));
  CHECK_FALSE(wire::isValidId(""));
  CHECK_FALSE(wire::isValidId(nullptr));
  CHECK_FALSE(wire::isValidId("bad id"));   // space
  CHECK_FALSE(wire::isValidId("bad!"));     // punctuation
  std::string tooLong(65, 'a');
  CHECK_FALSE(wire::isValidId(tooLong.c_str()));
  std::string maxLen(64, 'a');
  CHECK(wire::isValidId(maxLen.c_str()));
}

// --- wire parse/build round-trips ---
TEST_CASE("wire: parse a valid req and reject non-objects") {
  uint8_t abuf[4096];
  FixedArena arena(abuf, sizeof(abuf));
  ArduinoJson::JsonDocument doc(&arena);
  wire::Parsed p;

  std::string req = "{\"v\":1,\"t\":\"req\",\"id\":\"a\",\"p\":{\"x\":1}}";
  REQUIRE(wire::parse(doc, arena, reinterpret_cast<const uint8_t*>(req.data()), req.size(), p));
  CHECK(p.v == 1);
  CHECK(p.t == wire::MsgType::Req);
  CHECK(std::string(p.id) == "a");
  CHECK(p.hasP);

  std::string notObj = "[1,2,3]";
  CHECK_FALSE(wire::parse(doc, arena, reinterpret_cast<const uint8_t*>(notObj.data()), notObj.size(), p));
  std::string garbage = "not json {{{";
  CHECK_FALSE(wire::parse(doc, arena, reinterpret_cast<const uint8_t*>(garbage.data()), garbage.size(), p));
}

TEST_CASE("wire: build hb/rak and re-parse") {
  uint8_t out[256];
  uint8_t abuf[4096];
  FixedArena arena(abuf, sizeof(abuf));
  size_t n = wire::buildHeartbeat(out, sizeof(out), arena, "cmd1");
  REQUIRE(n > 0);
  ArduinoJson::JsonDocument doc(&arena);
  wire::Parsed p;
  REQUIRE(wire::parse(doc, arena, out, n, p));
  CHECK(p.v == 1);
  CHECK(p.t == wire::MsgType::Hb);
  CHECK(std::string(p.id) == "cmd1");
}

// --- JSON comparator semantics (must match the frozen C# JsonComparer) ---
TEST_CASE("jsonEqual: number/bool/null/nesting semantics") {
  uint8_t abuf[4096];
  FixedArena arena(abuf, sizeof(abuf));

  auto eq = [&](const char* sa, const char* sb) -> bool {
    ArduinoJson::JsonDocument da(&arena), db(&arena);
    ArduinoJson::deserializeJson(da, sa);
    ArduinoJson::deserializeJson(db, sb);
    return jsonEqual(da.as<ArduinoJson::JsonVariantConst>(), db.as<ArduinoJson::JsonVariantConst>());
  };

  CHECK(eq("1", "1.0"));                 // int == float
  CHECK(eq("3.14", "3.14"));             // exact double (USE_DOUBLE)
  CHECK_FALSE(eq("true", "1"));          // bool != number
  CHECK_FALSE(eq("1", "true"));
  CHECK(eq("true", "true"));
  CHECK_FALSE(eq("{\"x\":null}", "{}"));            // null value vs absent key are distinct
  CHECK(eq("{\"x\":null}", "{\"x\":null}"));
  CHECK(eq("{\"a\":1,\"b\":[1,2,3]}", "{\"b\":[1,2,3],\"a\":1}"));  // key order insignificant
  CHECK_FALSE(eq("{\"a\":1}", "{\"a\":1,\"b\":2}"));
  CHECK(eq("\"\\u3053\\u3093\"", "\"こん\""));       // UTF-8 fidelity
}

// --- sustained load: bounded state + zero heap in the core steady state ---
namespace {
template <int N>
class AutoHandler : public parley::ParleyCommandHandler<int, int, int, RawCode, N, 256> {
 public:
  typedef parley::ParleyCommandHandler<int, int, int, RawCode, N, 256> Base;
  typedef parley::ParleyExecution<int, int, int, RawCode> Exec;
  AutoHandler(parley::ParleyConnectionAdapter& a, const parley::ParleyTopics& t) : Base(a, t) {}
 protected:
  void handleCommand(Exec& e) override { e.succeed(e.request()); }  // synchronous, no heap
};

struct SinkCap {
  parley::ParleyResult<int, RawCode> result;
};
void sink(void* ud, const parley::ParleyResult<int, RawCode>& r) {
  static_cast<SinkCap*>(ud)->result = r;
}
}  // namespace

TEST_CASE("sustained load (commander): bounded state + zero heap in the core steady-state window") {
  SilentAdapter adapter;
  CountingIdGenerator idgen;
  parley::ParleyCommander<int, int, int, RawCode, 4, 256> c(adapter, unitTopics(), parley::ParleyConfig(),
                                                            &idgen);
  char buf[128];
  SinkCap cap;

  // Warmup one full cycle so any lazy static init happens before the heap snapshot.
  {
    c.execute(0, &sink, &cap);
    int n = snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"res\",\"id\":\"u0\",\"ok\":true,\"p\":1}");
    c.deliver("result", reinterpret_cast<const uint8_t*>(buf), n);
  }

  long a0 = parleyAllocCount();
  const int cycles = 5000;
  for (int i = 1; i <= cycles; ++i) {
    c.execute(0, &sink, &cap);  // CountingIdGenerator yields "u<i>"
    int n = snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"res\",\"id\":\"u%d\",\"ok\":true,\"p\":1}", i);
    c.deliver("result", reinterpret_cast<const uint8_t*>(buf), n);
  }
  long a1 = parleyAllocCount();

  CHECK_MESSAGE(a1 == a0, "core steady state allocated " << (a1 - a0) << " times (expected 0)");
  CHECK(c.inflightCount() == 0);
  CHECK(c.tombstoneCount() <= 16);  // bounded by MaxTombstones (MaxInFlight*4)

  c.tick(200000);  // past tombstoneTtl → tombstones expire
  CHECK(c.tombstoneCount() == 0);
  CHECK(c.activeTimerCount() == 0);
}

TEST_CASE("sustained load (handler): bounded state + zero heap in the core steady-state window") {
  SilentAdapter adapter;
  AutoHandler<4> h(adapter, unitTopics());
  char buf[128];

  {  // warmup
    int n = snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"req\",\"id\":\"w\",\"p\":1}");
    h.deliver("request", reinterpret_cast<const uint8_t*>(buf), n);
    n = snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"rak\",\"id\":\"w\"}");
    h.deliver("request", reinterpret_cast<const uint8_t*>(buf), n);
  }

  long a0 = parleyAllocCount();
  const int cycles = 5000;
  for (int i = 0; i < cycles; ++i) {
    int n = snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"req\",\"id\":\"h%d\",\"p\":1}", i);
    h.deliver("request", reinterpret_cast<const uint8_t*>(buf), n);  // admit → auto-succeed → cache
    n = snprintf(buf, sizeof(buf), "{\"v\":1,\"t\":\"rak\",\"id\":\"h%d\"}", i);
    h.deliver("request", reinterpret_cast<const uint8_t*>(buf), n);  // → tombstone
  }
  long a1 = parleyAllocCount();

  CHECK_MESSAGE(a1 == a0, "core steady state allocated " << (a1 - a0) << " times (expected 0)");
  CHECK(h.commandCount() == 0);
  CHECK(h.tombstoneCount() <= 16);

  h.tick(200000);
  CHECK(h.tombstoneCount() == 0);
  CHECK(h.activeTimerCount() == 0);
}
