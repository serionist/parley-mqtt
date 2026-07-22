#ifndef PARLEY_COMMAND_HANDLER_H
#define PARLEY_COMMAND_HANDLER_H

#include <stddef.h>
#include <stdint.h>

#include "ParleyCodec.h"
#include "ParleyConfig.h"
#include "ParleyConnectionAdapter.h"
#include "ParleyExecution.h"
#include "ParleyTopics.h"
#include "detail/handler_core.h"
#include "detail/json.h"
#include "detail/limits.h"

namespace parley {

// The callee side of one command type (protocol.md §7, H1–H8). The consumer overrides handleCommand.
// A topic set MUST have at most one active handler. All state is statically allocated; drive it from
// loop() via tick(millis()). The heavy state machine lives in the shared HandlerCore (plan F9); this
// shell only decodes the request and constructs the typed execution.
//
//   MaxCommands   — admission cap (protocol.md maxActiveCommands; the pool IS the cap — design.md)
//   MaxPayload    — serialized envelope byte cap; bounds outbound caches AND inbound parse (plan F2)
//   MaxTombstones — bounded tombstone table (H5/H8); oldest-first eviction beyond it
template <class Req, class Prog, class Res, class Err, int MaxCommands, int MaxPayload = 256,
          int MaxTombstones = MaxCommands * 4>
class ParleyCommandHandler : public detail::HandlerHost {
 public:
  static_assert(MaxCommands >= 1, "MaxCommands must be >= 1");
  static_assert(MaxPayload <= 65535, "MaxPayload must fit the uint16 wire length fields (MS3)");
  static_assert(MaxTombstones >= 1, "MaxTombstones must be >= 1 (MS2)");

  ParleyCommandHandler(ParleyConnectionAdapter& adapter, const ParleyTopics& topics,
                       const ParleyConfig& config = ParleyConfig()) {
    // CFG: validate (clamp out-of-range durations/caps to defaults) at construction — a zero
    // self-rearming interval would otherwise make tick() spin forever.
    core_.init(&adapter, topics, detail::validatedConfig(config), this, slots_, MaxCommands,
               &resCaches_[0][0], MaxPayload, scratch_, tombIds_, tombDue_, MaxTombstones, parseArena_,
               kArenaCap, buildArena_, kArenaCap);
  }

  virtual ~ParleyCommandHandler() { core_.disposeAll(); }

  ParleyCommandHandler(const ParleyCommandHandler&) = delete;
  ParleyCommandHandler& operator=(const ParleyCommandHandler&) = delete;

  void deliver(const char* topic, const uint8_t* payload, size_t length) {
    core_.deliver(topic, payload, length);
  }
  void connected() { core_.connected(); }   // (re)connection signal → resubscribe (§2)
  void tick(uint32_t nowMs) { core_.tick(nowMs); }

  // --- inspection hooks for the bounded-state tests ---
  int commandCount() const { return core_.commandCount(); }
  int queueCount() const { return core_.queueCount(); }
  int executingCount() const { return core_.executingCount(); }
  int tombstoneCount() const { return core_.tombstoneCount(); }
  int activeTimerCount() const { return core_.activeTimerCount(); }

 protected:
  // The consumer's command implementation. Return quickly on the dispatch path (heavy synchronous work
  // delays other commands' heartbeats); stash `&execution` and finish across later tick()s if needed.
  virtual void handleCommand(ParleyExecution<Req, Prog, Res, Err>& execution) = 0;

  // detail::HandlerHost
  bool decodeRequest(int slot, ArduinoJson::JsonVariantConst p, bool pPresent) override {
    if (!pPresent) {
      requestStorage_[slot] = Req();  // §4: no payload ≡ the empty value
      return true;
    }
    Req value = Req();
    if (!ParleyCodec<Req>::decode(p, value)) return false;  // §4: undecodable req → silent drop
    requestStorage_[slot] = value;
    return true;
  }

  void dispatch(int slot, uint32_t gen) override {
    execs_[slot].bind(&core_, slot, gen, &requestStorage_[slot]);
    handleCommand(execs_[slot]);
  }

 private:
  static const size_t kArenaCap = static_cast<size_t>(MaxPayload) * 16 + 1024;

  detail::HandlerCore core_;
  detail::HandlerSlot slots_[MaxCommands];
  uint8_t resCaches_[MaxCommands][MaxPayload];
  uint8_t scratch_[MaxPayload];
  char tombIds_[MaxTombstones][kIdCapacity];
  uint32_t tombDue_[MaxTombstones];
  uint8_t parseArena_[kArenaCap];
  uint8_t buildArena_[kArenaCap];
  Req requestStorage_[MaxCommands];
  ParleyExecution<Req, Prog, Res, Err> execs_[MaxCommands];
};

}  // namespace parley

#endif  // PARLEY_COMMAND_HANDLER_H
