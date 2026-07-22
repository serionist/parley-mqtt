#ifndef PARLEY_DETAIL_TIME_UTIL_H
#define PARLEY_DETAIL_TIME_UTIL_H

#include <stdint.h>

// Wraparound-safe millisecond time (protocol.md §8 durations; ESP32 millis() is a uint32_t that
// rolls over every ~49.7 days). All time comparisons go through signed-difference helpers so a
// rollover between an arm and its due instant compares correctly. See CONVENTIONS.md (C++ time).

namespace parley {
namespace detail {

// Sentinel duration meaning "no timeout / disabled". A duration equal to this is NEVER added to a
// start instant (that would be a wraparound mis-fire trap, plan F8) — callers take an explicit
// skip-scheduling branch instead.
static const uint32_t kNoTimeout = 0xFFFFFFFFu;

// Scheduling horizon (MS4): every configured duration must stay strictly below 2^31 ms (~24.8 days,
// half the uint32 ms range). timeReached()/timeBefore() are two's-complement signed diffs that stay
// correct only while the arm→due gap is under half the range; a duration >= 2^31 would flip the sign
// and mis-fire. ParleyConfig validation (detail::validatedConfig) clamps any duration at or above this
// bound back to its default, so no timer is ever armed beyond the horizon.
static const uint32_t kMaxDuration = 0x80000000u;

// True when `now` has reached or passed `due`, correct across a single uint32 rollover, provided the
// scheduling horizon stays under ~24.8 days (half the uint32 ms range) — far beyond every Parley
// interval. Implemented as a signed difference so the wrap is handled by two's-complement subtraction.
inline bool timeReached(uint32_t now, uint32_t due) {
  return static_cast<int32_t>(now - due) >= 0;
}

// True when instant `a` is strictly before instant `b` (signed-diff, wraparound-safe).
inline bool timeBefore(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) < 0;
}

}  // namespace detail
}  // namespace parley

#endif  // PARLEY_DETAIL_TIME_UTIL_H
