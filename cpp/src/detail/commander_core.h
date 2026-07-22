#ifndef PARLEY_DETAIL_COMMANDER_CORE_H
#define PARLEY_DETAIL_COMMANDER_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "../ParleyCallOptions.h"
#include "../ParleyConfig.h"
#include "../ParleyConnectionAdapter.h"
#include "../ParleyError.h"
#include "../ParleyIdGenerator.h"
#include "../ParleyTopics.h"
#include "core_util.h"
#include "json.h"
#include "wire.h"

// The non-templated commander state machine (protocol.md §6, C1–C8). Compiled once regardless of the
// consumer's payload types (plan F9). The typed shell injects request encoding (a wire::PayloadWriter)
// and receives terminal/progress delivery through CommanderHost, which identifies calls by slot index.

namespace parley {
namespace detail {

// Terminal/progress delivery back to the typed shell. Called synchronously from deliver()/tick()/
// disposeAll(); the slot index keys the shell's per-call typed callback storage.
class CommanderHost {
 public:
  virtual ~CommanderHost() {}
  // Decodes and delivers a success payload. Returns false when the payload does not decode to the
  // result type — the core then delivers $decodeError (the rak was already sent, C4).
  virtual bool onSuccess(int slot, ArduinoJson::JsonVariantConst p, bool pPresent) = 0;
  // Delivers an error terminal. `code` is the wire e-string (builtin or custom); `m` may be null.
  virtual void onError(int slot, const char* code, const char* m) = 0;
  // Delivers an advisory progress payload; a decode failure is dropped silently by the shell (C8).
  virtual void onProgress(int slot, ArduinoJson::JsonVariantConst p, bool pPresent) = 0;
};

struct CommanderSlot {
  bool inUse;
  uint32_t gen;
  char id[kIdCapacity];
  uint8_t state;  // kSending / kLive
  bool cancelling;
  uint8_t* reqBuf;  // → shell-owned cache row (MaxPayload bytes)
  uint16_t reqLen;
  uint16_t reqCap;
  uint32_t requestRetryInterval;
  uint32_t noAckTimeout;
  uint32_t staleTimeout;
  uint32_t executionTimeout;  // kNoTimeout = disabled
  uint32_t cancelRetryInterval;
  uint32_t tombstoneTtl;
  Timer requestRetry;
  Timer signOfLife;
  Timer executionCap;
  Timer cancelRetry;

  CommanderSlot() : inUse(false), gen(0), state(0), cancelling(false), reqBuf(nullptr), reqLen(0),
                    reqCap(0), requestRetryInterval(0), noAckTimeout(0), staleTimeout(0),
                    executionTimeout(0), cancelRetryInterval(0), tombstoneTtl(0) {
    id[0] = '\0';
  }
};

class CommanderCore {
 public:
  static const uint8_t kSending = 0;
  static const uint8_t kLive = 1;

  CommanderCore();

  void init(ParleyConnectionAdapter* adapter, const ParleyTopics& topics, const ParleyConfig& config,
            ParleyIdGenerator* idgen, CommanderHost* host, CommanderSlot* slots, int slotCount,
            uint8_t* reqCaches, size_t maxPayload, uint8_t* scratch, char (*tombIds)[kIdCapacity],
            uint32_t* tombDue, int tombCap, uint8_t* parseArena, size_t parseArenaCap,
            uint8_t* buildArena, size_t buildArenaCap);

  // Reserves a call slot and generates its id; returns the slot index or -1 (pool exhausted, F7).
  int reserveCall(const ParleyCallOptions& opts);
  // Builds + publishes the request for a reserved slot; returns an invalid handle on encode/oversize
  // failure (the slot is released — a synchronous local failure, F7).
  ParleyCallHandle sendCall(int slot, wire::PayloadWriter writer, void* writerCtx);
  void cancel(ParleyCallHandle handle);
  void deliver(const char* topic, const uint8_t* payload, size_t len);
  void connected();
  void tick(uint32_t now);
  void disposeAll();

  const char* slotId(int slot) const { return slots_[slot].id; }

  int inflightCount() const;
  int tombstoneCount() const { return tombstones_.count(); }
  int activeTimerCount() const;

 private:
  void subscribeAll();
  int findInflight(const char* id) const;
  void onHeartbeat(const char* id);
  void onProgressMsg(const char* id, ArduinoJson::JsonVariantConst p, bool hasP);
  void onResult(const wire::Parsed& p);
  void onSignOfLife(int slot);
  void requestCancel(int slot);
  void terminateSuccess(int slot, ArduinoJson::JsonVariantConst p, bool hasP);
  void terminateError(int slot, const char* code, const char* m, bool isBusy);
  void terminateBuiltin(int slot, const char* wireCode);
  void freeSlot(int slot);
  void publishRak(const char* id);
  void publishCnl(const char* id);
  void publish(const char* topic, const uint8_t* bytes, size_t len);
  bool fireEarliestDue(uint32_t now);

  ParleyConnectionAdapter* adapter_;
  ParleyTopics topics_;
  ParleyConfig config_;
  ParleyIdGenerator* idgen_;
  CommanderHost* host_;
  CommanderSlot* slots_;
  int slotCount_;
  size_t maxPayload_;
  uint8_t* scratch_;
  TombstoneStore tombstones_;
  uint8_t* parseArenaBuf_;
  size_t parseArenaCap_;
  uint8_t* buildArenaBuf_;
  size_t buildArenaCap_;
  uint32_t now_;
};

}  // namespace detail
}  // namespace parley

#endif  // PARLEY_DETAIL_COMMANDER_CORE_H
