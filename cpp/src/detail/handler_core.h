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
  // Dispatches an admitted, dequeued command to the consumer's handleCommand (H4). `gen` stamps the
  // slot's admission generation into the typed execution so a stale handle (slot reused) no-ops.
  virtual void dispatch(int slot, uint32_t gen) = 0;
};

struct HandlerSlot {
  bool inUse;
  uint32_t gen;  // admission generation — bumped on every reserve; gen-stamps the handler's execution
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

  HandlerSlot() : inUse(false), gen(0), state(0), cancelRequested(false), seq(0), resBuf(nullptr),
                  resLen(0), resCap(0) {
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
  // Every exec-forwarded op carries the admission `gen` captured at bind time and no-ops unless the
  // slot is still in use, still on that generation, AND still EXECUTING (MS1/HC1). This mirrors the
  // commander's gen-stamped ParleyCallHandle: a stale/duplicate call after a tombstone+slot-reuse (or
  // a second succeed()/fail() on an already-completed command) must never publish for another command
  // nor corrupt executingCount_.
  void completeSuccess(int slot, uint32_t gen, wire::PayloadWriter writer, void* ctx);
  void completeCustomError(int slot, uint32_t gen, const char* code, const char* m);
  void completeCancelled(int slot, uint32_t gen);
  void abort(int slot, uint32_t gen);
  void emitProgress(int slot, uint32_t gen, wire::PayloadWriter writer, void* ctx);
  bool isCancelRequested(int slot, uint32_t gen) const;
  const char* slotId(int slot, uint32_t gen) const;

  int commandCount() const;
  int queueCount() const;
  int executingCount() const { return executingCount_; }
  int tombstoneCount() const { return tombstones_.count(); }
  int activeTimerCount() const;

 private:
  void subscribeAll();
  int findCommand(const char* id) const;
  int reserveSlot();
  // True iff `slot` is a live, EXECUTING command still on generation `gen` — the guard shared by every
  // exec-forwarded op. A completed command (kResultCached) or a reused slot (gen bumped) fails it.
  bool execIsLive(int slot, uint32_t gen) const;
  void onRequest(const char* id, ArduinoJson::JsonVariantConst p, bool hasP);
  void onResultAck(const char* id);
  void onCancel(const char* id);
  void tryDispatch();
  int pickQueued() const;
  // Internal completion workers (no gen guard — callers hold a known-live slot).
  void finishCancelled(int slot);
  void abortSlot(int slot);
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
