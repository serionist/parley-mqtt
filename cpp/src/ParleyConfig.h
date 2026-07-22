#ifndef PARLEY_CONFIG_H
#define PARLEY_CONFIG_H

#include <stdint.h>

#include "detail/time_util.h"

namespace parley {

// Per-deployment timing configuration and handler capacity caps (protocol.md §8). Durations are in
// milliseconds. Not negotiated over the wire; each peer is configured independently. Defaults match
// protocol.md §8. maxActiveCommands is NOT stored here for C++ — the pool template parameter
// (MaxCommands) is the admission cap (design.md); maxConcurrentExecutions still applies.
struct ParleyConfig {
  uint32_t requestRetryInterval;  // C1
  uint32_t noAckTimeout;          // C3 (SENDING window)
  uint32_t staleTimeout;          // C3 (LIVE sliding window)
  uint32_t executionTimeout;      // C2/C3 overall cap; detail::kNoTimeout disables it
  uint32_t cancelRetryInterval;   // C7
  uint32_t heartbeatInterval;     // H3
  uint32_t resultRetryInterval;   // H5
  uint32_t resultRetryTimeout;    // H5 (from first res publication)
  uint32_t tombstoneTtl;          // C5 / H5
  int maxConcurrentExecutions;    // H4 (default 1, serial)

  ParleyConfig()
      : requestRetryInterval(2000),
        noAckTimeout(10000),
        staleTimeout(15000),
        executionTimeout(60000),
        cancelRetryInterval(2000),
        heartbeatInterval(3000),
        resultRetryInterval(2000),
        resultRetryTimeout(30000),
        tombstoneTtl(60000),
        maxConcurrentExecutions(1) {}
};

namespace detail {

// Config-validation policy (CFG / MS4). Runtime local fail-fast in the no-exceptions house style: a
// constructor cannot return a sentinel, and the house avoids runtime <cassert> (assert would abort a
// release device), so the chosen idiom is a *validated() factory that CLAMPS every out-of-range field
// to its protocol.md §8 default. Rationale:
//   - a self-rearming interval of 0 is immediately-due and re-arms with delay 0 → tick() spins forever
//     (the motivating hang); a one-shot window of 0 fires instantly. Both are misconfigurations.
//   - a duration >= 2^31 ms breaks the wraparound-safe timer horizon (time_util.h kMaxDuration).
//   - maxConcurrentExecutions < 1 would never dispatch (a silent deadlock).
// Clamping (rather than aborting) keeps a mis-provisioned device memory-safe and live on its defaults;
// the policy is deterministic and unit-tested. executionTimeout additionally accepts kNoTimeout, which
// is a disable sentinel (never armed) and passes through untouched.
inline uint32_t clampDuration(uint32_t v, uint32_t def) {
  return (v == 0 || v >= kMaxDuration) ? def : v;
}

inline ParleyConfig validatedConfig(const ParleyConfig& in) {
  const ParleyConfig def;  // protocol.md §8 defaults
  ParleyConfig out = in;
  out.requestRetryInterval = clampDuration(in.requestRetryInterval, def.requestRetryInterval);
  out.noAckTimeout = clampDuration(in.noAckTimeout, def.noAckTimeout);
  out.staleTimeout = clampDuration(in.staleTimeout, def.staleTimeout);
  out.executionTimeout = (in.executionTimeout == kNoTimeout)
                             ? kNoTimeout
                             : clampDuration(in.executionTimeout, def.executionTimeout);
  out.cancelRetryInterval = clampDuration(in.cancelRetryInterval, def.cancelRetryInterval);
  out.heartbeatInterval = clampDuration(in.heartbeatInterval, def.heartbeatInterval);
  out.resultRetryInterval = clampDuration(in.resultRetryInterval, def.resultRetryInterval);
  out.resultRetryTimeout = clampDuration(in.resultRetryTimeout, def.resultRetryTimeout);
  out.tombstoneTtl = clampDuration(in.tombstoneTtl, def.tombstoneTtl);
  if (in.maxConcurrentExecutions < 1) out.maxConcurrentExecutions = def.maxConcurrentExecutions;
  return out;
}

}  // namespace detail

}  // namespace parley

#endif  // PARLEY_CONFIG_H
