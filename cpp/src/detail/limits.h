#ifndef PARLEY_DETAIL_LIMITS_H
#define PARLEY_DETAIL_LIMITS_H

#include <stddef.h>

// Fixed sizes shared across the core and the typed shell. All per-command state is statically sized
// from these plus the MaxPayload / MaxInFlight / MaxCommands template parameters.

namespace parley {

// Command-id buffer: 64 chars (protocol.md §4.2 upper bound) + NUL.
static const size_t kIdCapacity = 65;

// Error-code wire string buffer (`e`). Longest builtin is "$executionTimeout" (17); custom codes are
// bounded here — a longer consumer code is rejected at encode time (fail-fast), never truncated.
static const size_t kCodeCapacity = 48;

// Human-readable error detail (`m`) buffer. Advisory; a longer value is truncated (plan §6 — no vector
// asserts `m`).
static const size_t kMessageCapacity = 96;

}  // namespace parley

#endif  // PARLEY_DETAIL_LIMITS_H
