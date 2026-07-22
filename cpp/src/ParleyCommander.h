#ifndef PARLEY_COMMANDER_H
#define PARLEY_COMMANDER_H

#include <stddef.h>
#include <stdint.h>

#include "ParleyCallOptions.h"
#include "ParleyCodec.h"
#include "ParleyConfig.h"
#include "ParleyConnectionAdapter.h"
#include "ParleyError.h"
#include "ParleyIdGenerator.h"
#include "ParleyTopics.h"
#include "detail/commander_core.h"
#include "detail/core_util.h"
#include "detail/fixed_arena.h"
#include "detail/json.h"
#include "detail/limits.h"

namespace parley {

using detail::ParleyCallHandle;

// The caller side of one command type (protocol.md §6, C1–C8). Generic over the request, progress,
// result, and custom-error types; the pool sizes are template parameters (design.md — "template
// parameters carry the types AND the pool sizes"). All state is statically allocated; drive it from
// loop() via tick(millis()). The heavy state machine lives in the shared CommanderCore (plan F9); this
// shell only encodes the request and decodes/dispatches typed callbacks.
//
//   MaxInFlight   — concurrent in-flight calls (the static-allocation cap; NOT a protocol cap)
//   MaxPayload    — serialized envelope byte cap; bounds outbound caches AND inbound parse (plan F2)
//   MaxTombstones — bounded tombstone table (C5); oldest-first eviction beyond it
template <class Req, class Prog, class Res, class Err, int MaxInFlight, int MaxPayload = 256,
          int MaxTombstones = MaxInFlight * 4>
class ParleyCommander : public detail::CommanderHost {
 public:
  typedef void (*ProgressCallback)(void* userData, const Prog& progress);
  typedef void (*CompleteCallback)(void* userData, const ParleyResult<Res, Err>& result);

  static_assert(MaxInFlight >= 1, "MaxInFlight must be >= 1");
  static_assert(MaxPayload <= 65535, "MaxPayload must fit the uint16 wire length fields (MS3)");
  static_assert(MaxTombstones >= 1, "MaxTombstones must be >= 1 (MS2)");

  ParleyCommander(ParleyConnectionAdapter& adapter, const ParleyTopics& topics,
                  const ParleyConfig& config = ParleyConfig(), ParleyIdGenerator* idGenerator = nullptr)
      : idgen_(idGenerator ? idGenerator : &defaultIdGen_) {
    for (int i = 0; i < MaxInFlight; ++i) {
      progressCb_[i] = nullptr;
      completeCb_[i] = nullptr;
      userData_[i] = nullptr;
    }
    // CFG: validate (clamp out-of-range durations to defaults) at construction — a zero self-rearming
    // interval would otherwise make tick() spin forever.
    core_.init(&adapter, topics, detail::validatedConfig(config), idgen_, this, slots_, MaxInFlight,
               &reqCaches_[0][0], MaxPayload, scratch_, tombIds_, tombDue_, MaxTombstones, parseArena_,
               kArenaCap, buildArena_, kArenaCap);
  }

  ~ParleyCommander() { core_.disposeAll(); }

  ParleyCommander(const ParleyCommander&) = delete;
  ParleyCommander& operator=(const ParleyCommander&) = delete;

  // Start one call. Returns an invalid handle (call did NOT start) when the pool is full (F7) or the
  // request payload exceeds MaxPayload — a synchronous local failure, never a $ wire code.
  ParleyCallHandle execute(const Req& request, const ParleyCallOptions& options,
                           ProgressCallback onProgress, CompleteCallback onComplete, void* userData) {
    int slot = core_.reserveCall(options);
    if (slot < 0) return ParleyCallHandle::invalid();
    progressCb_[slot] = onProgress;
    completeCb_[slot] = onComplete;
    userData_[slot] = userData;
    return core_.sendCall(slot, &requestWriter, const_cast<Req*>(&request));
  }

  // Convenience: default options, no progress sink.
  ParleyCallHandle execute(const Req& request, CompleteCallback onComplete, void* userData = nullptr) {
    return execute(request, ParleyCallOptions(), nullptr, onComplete, userData);
  }

  void cancel(ParleyCallHandle handle) { core_.cancel(handle); }        // C7
  void deliver(const char* topic, const uint8_t* payload, size_t length) {
    core_.deliver(topic, payload, length);
  }
  void connected() { core_.connected(); }   // (re)connection signal → resubscribe (§2)
  void tick(uint32_t nowMs) { core_.tick(nowMs); }

  // --- inspection hooks for the bounded-state tests ---
  int inflightCount() const { return core_.inflightCount(); }
  int tombstoneCount() const { return core_.tombstoneCount(); }
  int activeTimerCount() const { return core_.activeTimerCount(); }

 protected:
  // detail::CommanderHost
  bool onSuccess(int slot, ArduinoJson::JsonVariantConst p, bool pPresent) override {
    Res value = Res();
    if (pPresent && !ParleyCodec<Res>::decode(p, value)) return false;  // → $decodeError (C4)
    if (completeCb_[slot]) completeCb_[slot](userData_[slot], ParleyResult<Res, Err>::success(value));
    return true;
  }

  void onError(int slot, const char* code, const char* m) override {
    ParleyError<Err> err = buildError(code, m);
    if (completeCb_[slot]) completeCb_[slot](userData_[slot], ParleyResult<Res, Err>::failure(err));
  }

  void onProgress(int slot, ArduinoJson::JsonVariantConst p, bool pPresent) override {
    if (!progressCb_[slot]) return;
    Prog value = Prog();
    if (pPresent && !ParleyCodec<Prog>::decode(p, value)) return;  // C8: advisory → drop silently
    progressCb_[slot](userData_[slot], value);
  }

 private:
  static const size_t kArenaCap = static_cast<size_t>(MaxPayload) * 16 + 1024;

  static bool requestWriter(void* ctx, ArduinoJson::JsonVariant dst) {
    return ParleyCodec<Req>::encode(*static_cast<const Req*>(ctx), dst);
  }

  ParleyError<Err> buildError(const char* code, const char* m) {
    ParleyBuiltinError b = builtinFromWire(code);
    if (b != ParleyBuiltinError::None) {
      ParleyError<Err> e = ParleyError<Err>::builtinError(b);
      e.setMessage(m, m != nullptr);
      return e;
    }
    // Custom code: reconstruct the typed Err from the wire string (best-effort, default on failure).
    Err custom = Err();
    uint8_t arenaBuf[128];
    detail::FixedArena arena(arenaBuf, sizeof(arenaBuf));
    ArduinoJson::JsonDocument doc(&arena);
    ArduinoJson::JsonVariant v = doc.to<ArduinoJson::JsonVariant>();
    if (v.set(code)) ParleyCodec<Err>::decode(v, custom);
    return ParleyError<Err>::customError(code, custom, m, m != nullptr);
  }

  ParleyDefaultIdGenerator defaultIdGen_;
  ParleyIdGenerator* idgen_;
  detail::CommanderCore core_;
  detail::CommanderSlot slots_[MaxInFlight];
  uint8_t reqCaches_[MaxInFlight][MaxPayload];
  uint8_t scratch_[MaxPayload];
  char tombIds_[MaxTombstones][kIdCapacity];
  uint32_t tombDue_[MaxTombstones];
  uint8_t parseArena_[kArenaCap];
  uint8_t buildArena_[kArenaCap];
  ProgressCallback progressCb_[MaxInFlight];
  CompleteCallback completeCb_[MaxInFlight];
  void* userData_[MaxInFlight];
};

}  // namespace parley

#endif  // PARLEY_COMMANDER_H
