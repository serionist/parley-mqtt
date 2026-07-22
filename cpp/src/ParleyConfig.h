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

}  // namespace parley

#endif  // PARLEY_CONFIG_H
