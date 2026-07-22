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
