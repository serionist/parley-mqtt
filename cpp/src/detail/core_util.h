#ifndef PARLEY_DETAIL_CORE_UTIL_H
#define PARLEY_DETAIL_CORE_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "limits.h"
#include "time_util.h"

namespace parley {
namespace detail {

// A single tick-driven timer: an armed deadline. tick(now) fires the earliest due timer. Reschedule
// re-arms; disarm stops it. No callbacks here — the owning core dispatches by timer identity, which
// keeps the whole timer set in POD storage (zero heap, plan §3).
struct Timer {
  bool active;
  uint32_t due;

  Timer() : active(false), due(0) {}
  void arm(uint32_t now, uint32_t delay) {
    active = true;
    due = now + delay;
  }
  void disarm() { active = false; }
  bool dueAt(uint32_t now) const { return active && timeReached(now, due); }
};

// A bounded set of tombstoned command ids (protocol.md C5 / H5 / H8) over caller-owned fixed storage.
// Entries self-expire by TTL and the table is hard-capped with oldest-first (insertion-order)
// eviction, so state stays bounded on both axes. Insertion order is preserved in the array (index 0 is
// oldest) so overflow eviction and iteration are trivial.
class TombstoneStore {
 public:
  TombstoneStore() : ids_(nullptr), due_(nullptr), cap_(0), count_(0) {}

  void init(char (*ids)[kIdCapacity], uint32_t* due, int cap) {
    ids_ = ids;
    due_ = due;
    cap_ = cap;
    count_ = 0;
  }

  int count() const { return count_; }

  bool contains(const char* id) const { return indexOf(id) >= 0; }

  void add(const char* id, uint32_t nowDue) {
    // MS2: a zero/negative cap has no backing storage — adding would evict past index 0 and write
    // out of bounds. Guard here as well as at compile time (MaxTombstones >= 1 in the shells).
    if (cap_ <= 0) return;
    int existing = indexOf(id);
    if (existing >= 0) {
      due_[existing] = nowDue;  // refresh TTL (keep insertion position)
      return;
    }
    if (count_ >= cap_) {
      removeAt(0);  // oldest-first eviction
    }
    copyId(ids_[count_], id);
    due_[count_] = nowDue;
    ++count_;
  }

  void remove(const char* id) {
    int i = indexOf(id);
    if (i >= 0) removeAt(i);
  }

  // Removes every entry whose TTL has expired at `now`. Returns true if any were removed.
  bool expire(uint32_t now) {
    bool removed = false;
    int i = 0;
    while (i < count_) {
      if (timeReached(now, due_[i])) {
        removeAt(i);
        removed = true;
      } else {
        ++i;
      }
    }
    return removed;
  }

  // Earliest active TTL deadline; returns false if empty.
  bool earliestDue(uint32_t& out) const {
    if (count_ == 0) return false;
    out = due_[0];
    for (int i = 1; i < count_; ++i) {
      if (timeBefore(due_[i], out)) out = due_[i];
    }
    return true;
  }

  void clear() { count_ = 0; }

 private:
  int indexOf(const char* id) const {
    for (int i = 0; i < count_; ++i) {
      if (strcmp(ids_[i], id) == 0) return i;
    }
    return -1;
  }

  void removeAt(int i) {
    for (int j = i; j + 1 < count_; ++j) {
      copyId(ids_[j], ids_[j + 1]);
      due_[j] = due_[j + 1];
    }
    --count_;
  }

  static void copyId(char* dst, const char* src) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < kIdCapacity; ++i) dst[i] = src[i];
    dst[i] = '\0';
  }

  char (*ids_)[kIdCapacity];
  uint32_t* due_;
  int cap_;
  int count_;
};

// A commander call handle: a slot index plus a generation stamp so a stale handle (slot reused) is
// rejected by cancel(). An invalid handle (slot < 0) means the call did not start.
struct ParleyCallHandle {
  int32_t slot;
  uint32_t gen;

  ParleyCallHandle() : slot(-1), gen(0) {}
  ParleyCallHandle(int32_t s, uint32_t g) : slot(s), gen(g) {}
  bool valid() const { return slot >= 0; }
  static ParleyCallHandle invalid() { return ParleyCallHandle(); }
};

inline void copyIdInto(char* dst, size_t cap, const char* src) {
  size_t i = 0;
  if (src != nullptr) {
    for (; src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

}  // namespace detail
}  // namespace parley

#endif  // PARLEY_DETAIL_CORE_UTIL_H
