#ifndef PARLEY_DETAIL_LIMITS_H
#define PARLEY_DETAIL_LIMITS_H

#include <stddef.h>

// Fixed sizes shared across the core and the typed shell. All per-command state is statically sized
// from these plus the MaxPayload / MaxInFlight / MaxCommands template parameters.

namespace parley {

// Command-id buffer: 64 chars (protocol.md §4.2 upper bound) + NUL.
static const size_t kIdCapacity = 65;

// Error-code wire string buffer (`e`). Longest builtin is "$executionTimeout" (17). Two directions,
// two policies (AB2):
//   - Encode side (this device produces a custom code via ParleyExecution::fail): a code longer than
//     kCodeCapacity-1 is rejected fail-fast — the command aborts, exactly like the $-prefix path — so
//     we never emit a silently-shortened code onto the wire.
//   - Receive side (a peer sent us an oversize code): we cannot fail-fast a remote peer, so the code
//     surfaced to the consumer is truncated into this buffer. That truncation is advisory, like `m`;
//     no vector asserts a custom code longer than this bound.
static const size_t kCodeCapacity = 48;

// Human-readable error detail (`m`) buffer. Advisory; a longer value is truncated (plan §6 — no vector
// asserts `m`).
static const size_t kMessageCapacity = 96;

}  // namespace parley

#endif  // PARLEY_DETAIL_LIMITS_H
