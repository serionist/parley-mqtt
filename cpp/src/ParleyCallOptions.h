#ifndef PARLEY_CALL_OPTIONS_H
#define PARLEY_CALL_OPTIONS_H

#include <stdint.h>

#include "detail/time_util.h"

namespace parley {

// Per-call commander-local overrides (protocol.md §8 "commander-local values also per-call"). A field
// left at 0 inherits the ParleyConfig default. executionTimeout additionally accepts detail::kNoTimeout
// to DISABLE the execution cap for this call (an explicit skip-scheduling branch, never start+kNoTimeout
// arithmetic — plan F8).
struct ParleyCallOptions {
  uint32_t requestRetryInterval;  // 0 = inherit
  uint32_t noAckTimeout;          // 0 = inherit
  uint32_t staleTimeout;          // 0 = inherit
  uint32_t executionTimeout;      // 0 = inherit, kNoTimeout = disable, else the cap
  uint32_t cancelRetryInterval;   // 0 = inherit
  uint32_t tombstoneTtl;          // 0 = inherit

  ParleyCallOptions()
      : requestRetryInterval(0),
        noAckTimeout(0),
        staleTimeout(0),
        executionTimeout(0),
        cancelRetryInterval(0),
        tombstoneTtl(0) {}
};

}  // namespace parley

#endif  // PARLEY_CALL_OPTIONS_H
