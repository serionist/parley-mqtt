#ifndef PARLEY_TEST_FAKE_ADAPTER_H
#define PARLEY_TEST_FAKE_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "Parley.h"

// A fake in-memory transport for the tests: records every publish and subscribe. Inbound delivery and
// the connected signal are pushed into the library directly (commander/handler deliver()/connected()),
// so this adapter is purely an outbound recorder (the plan's C++ adapter is outbound-only). An optional
// synchronous-loopback hook re-enters the library from inside publish() to prove publish-comes-last.
struct OutboundAction {
  bool isSubscribe;
  std::string topic;
  std::vector<uint8_t> payload;
};

class FakeAdapter : public parley::ParleyConnectionAdapter {
 public:
  std::vector<OutboundAction> outbound;

  // Optional loopback: invoked from INSIDE publish() (after recording) with the published bytes, so a
  // test can synchronously re-enter the library mid-transition. Re-entrant publishes do not re-trigger
  // it (single level). fn(ctx, topic, payload, len).
  void (*onPublish)(void*, const char*, const uint8_t*, size_t) = nullptr;
  void* onPublishCtx = nullptr;

  // While > 0, publish() silently drops (records nothing) and decrements — simulates a transiently
  // disconnected client so tests can prove the retry cadence self-heals (retries are scheduled before
  // publish, so a dropped send never stalls the cadence).
  int dropPublishes = 0;

  void publish(const char* topic, const uint8_t* payload, size_t length) override {
    if (dropPublishes > 0) {
      --dropPublishes;
      return;
    }
    OutboundAction a;
    a.isSubscribe = false;
    a.topic = topic;
    a.payload.assign(payload, payload + length);
    outbound.push_back(a);
    if (onPublish != nullptr && !reacting_) {
      reacting_ = true;
      onPublish(onPublishCtx, topic, payload, length);
      reacting_ = false;
    }
  }

  void subscribe(const char* topic) override {
    OutboundAction a;
    a.isSubscribe = true;
    a.topic = topic;
    outbound.push_back(a);
  }

  void unsubscribe(const char* topic) override { (void)topic; }

  void clear() { outbound.clear(); }

  int publishCount() const {
    int n = 0;
    for (size_t i = 0; i < outbound.size(); ++i) {
      if (!outbound[i].isSubscribe) ++n;
    }
    return n;
  }

 private:
  bool reacting_ = false;
};

// A scripted command-id generator: returns queued ids in FIFO order, bypassing the random §4.2
// generator so vector runners use the ids the vector specifies (protocol.md §10).
class ScriptedIdGenerator : public parley::ParleyIdGenerator {
 public:
  void enqueue(const std::string& id) { ids_.push_back(id); }

  void next(char* out, size_t cap) override {
    const std::string& id = ids_.at(head_++);
    size_t i = 0;
    for (; i < id.size() && i + 1 < cap; ++i) out[i] = id[i];
    out[i] = '\0';
  }

 private:
  std::vector<std::string> ids_;
  size_t head_ = 0;
};

#endif  // PARLEY_TEST_FAKE_ADAPTER_H
