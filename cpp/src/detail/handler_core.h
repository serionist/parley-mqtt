#ifndef PARLEY_DETAIL_HANDLER_CORE_H
#define PARLEY_DETAIL_HANDLER_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "../ParleyConfig.h"
#include "../ParleyConnectionAdapter.h"
#include "../ParleyError.h"
#include "../ParleyTopics.h"
#include "core_util.h"
#include "json.h"
#include "wire.h"

// The non-templated handler state machine (protocol.md §7, H1–H8). Compiled once regardless of the
// consumer's payload types (plan F9). The typed shell decodes the request and dispatches to the
// consumer's handleCommand through HandlerHost; outcomes flow back in via complete*/emitProgress.

namespace parley {
namespace detail {

class HandlerHost {
 public:
  virtual ~HandlerHost() {}
  // Decodes+stores the request payload for a reserved slot. false → decode failure → silent drop (§4).
  virtual bool decodeRequest(int slot, ArduinoJson::JsonVariantConst p, bool pPresent) = 0;
  // Dispatches an admitted, dequeued command to the consumer's handleCommand (H4).
  virtual void dispatch(int slot) = 0;
};

struct HandlerSlot {
  bool inUse;
  char id[kIdCapacity];
  uint8_t state;  // kQueued / kExecuting / kResultCached
  bool cancelRequested;
  uint32_t seq;  // admission order (FIFO dispatch)
  uint8_t* resBuf;  // → shell-owned cache row (MaxPayload bytes)
  uint16_t resLen;
  uint16_t resCap;
  Timer heartbeat;
  Timer resultRetry;
  Timer resultRetryTimeout;

  HandlerSlot() : inUse(false), state(0), cancelRequested(false), seq(0), resBuf(nullptr), resLen(0),
                  resCap(0) {
    id[0] = '\0';
  }
};

class HandlerCore {
 public:
  static const uint8_t kQueued = 0;
  static const uint8_t kExecuting = 1;
  static const uint8_t kResultCached = 2;

  HandlerCore();

  void init(ParleyConnectionAdapter* adapter, const ParleyTopics& topics, const ParleyConfig& config,
            HandlerHost* host, HandlerSlot* slots, int slotCount, uint8_t* resCaches,
            size_t maxPayload, uint8_t* scratch, char (*tombIds)[kIdCapacity], uint32_t* tombDue,
            int tombCap, uint8_t* parseArena, size_t parseArenaCap, uint8_t* buildArena,
            size_t buildArenaCap);

  void deliver(const char* topic, const uint8_t* payload, size_t len);
  void connected();
  void tick(uint32_t now);
  void disposeAll();

  // --- called by the shell's ParleyExecution ---
  void completeSuccess(int slot, wire::PayloadWriter writer, void* ctx);
  void completeCustomError(int slot, const char* code, const char* m);
  void completeCancelled(int slot);
  void abort(int slot);
  void emitProgress(int slot, wire::PayloadWriter writer, void* ctx);
  bool isCancelRequested(int slot) const { return slots_[slot].cancelRequested; }
  const char* slotId(int slot) const { return slots_[slot].id; }

  int commandCount() const;
  int queueCount() const;
  int executingCount() const { return executingCount_; }
  int tombstoneCount() const { return tombstones_.count(); }
  int activeTimerCount() const;

 private:
  void subscribeAll();
  int findCommand(const char* id) const;
  int reserveSlot();
  void onRequest(const char* id, ArduinoJson::JsonVariantConst p, bool hasP);
  void onResultAck(const char* id);
  void onCancel(const char* id);
  void tryDispatch();
  int pickQueued() const;
  void completeWithResult(int slot, size_t resLen, bool wasExecuting);
  void moveToTombstone(int slot);
  void publishHeartbeat(int slot);
  void republishCachedResult(int slot);
  void publishStateless(const char* id, const char* code);
  void releaseSlot(int slot);
  void publish(const char* topic, const uint8_t* bytes, size_t len);
  bool fireEarliestDue(uint32_t now);

  ParleyConnectionAdapter* adapter_;
  ParleyTopics topics_;
  ParleyConfig config_;
  HandlerHost* host_;
  HandlerSlot* slots_;
  int slotCount_;
  size_t maxPayload_;
  uint8_t* scratch_;
  TombstoneStore tombstones_;
  uint8_t* parseArenaBuf_;
  size_t parseArenaCap_;
  uint8_t* buildArenaBuf_;
  size_t buildArenaCap_;
  uint32_t now_;
  uint32_t nextSeq_;
  int executingCount_;
  bool dispatching_;
};

}  // namespace detail
}  // namespace parley

#endif  // PARLEY_DETAIL_HANDLER_CORE_H
