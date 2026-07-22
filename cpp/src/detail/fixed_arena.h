#ifndef PARLEY_DETAIL_FIXED_ARENA_H
#define PARLEY_DETAIL_FIXED_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "json.h"

// A fixed-capacity, zero-heap allocation strategy for ArduinoJson v7 (plan §3 memory model). The
// JsonDocument is handed a FixedArena backed by a caller-owned static byte buffer; every allocation
// bumps a pointer within that buffer. When the buffer is exhausted allocate() returns nullptr, which
// ArduinoJson reports as a NoMemory error on parse / overflowed() on build — exactly the "inbound
// oversize is unparseable and silently dropped" and "outbound oversize fails fast" postures (F2).
//
// The arena is reset (`reset()`) at the start of every wire operation, so a single fixed buffer serves
// an unbounded number of parse/build cycles with no heap and no growth. ArduinoJson grows its variant/
// string pools via reallocate(): the common case reallocates the most-recently-allocated block, which
// this arena resizes IN PLACE (no copy, no wasted space). A reallocate of an older block falls back to
// a fresh block + copy (rare). Each block carries a size header so that fallback knows how much to copy.

namespace parley {
namespace detail {

class FixedArena : public ArduinoJson::Allocator {
 public:
  FixedArena(uint8_t* buffer, size_t capacity) : used_(0), lastData_(nullptr), lastBase_(0) {
    // Align the base to kAlign so every block (and its size header) is naturally aligned regardless of
    // the caller's buffer alignment — the buffers are uint8_t[] (alignment 1). With an aligned base and
    // kAlign-multiple block sizes, both the size headers and the returned data satisfy ArduinoJson's
    // alignment requirements.
    uintptr_t raw = reinterpret_cast<uintptr_t>(buffer);
    uintptr_t aligned = (raw + (kAlign - 1)) & ~static_cast<uintptr_t>(kAlign - 1);
    size_t adjust = static_cast<size_t>(aligned - raw);
    buffer_ = reinterpret_cast<uint8_t*>(aligned);
    capacity_ = capacity > adjust ? capacity - adjust : 0;
  }

  // Discard every outstanding allocation. Call before each parse/build cycle.
  void reset() {
    used_ = 0;
    lastData_ = nullptr;
  }

  void* allocate(size_t size) override {
    size_t base = used_;
    size_t need = kAlign + roundUp(size);
    if (need > capacity_ - used_) return nullptr;  // exhausted → ArduinoJson NoMemory / overflowed()
    uint8_t* p = buffer_ + base;
    *reinterpret_cast<size_t*>(p) = size;
    used_ += need;
    lastData_ = p + kAlign;
    lastBase_ = base;
    return lastData_;
  }

  void deallocate(void* ptr) override {
    (void)ptr;  // arena: reclaimed only on reset()
  }

  void* reallocate(void* ptr, size_t new_size) override {
    if (ptr == nullptr) return allocate(new_size);
    if (ptr == lastData_) {
      // Top block: resize in place. Data stays put; only the tail high-water mark moves.
      size_t need = kAlign + roundUp(new_size);
      if (lastBase_ + need > capacity_) return nullptr;
      used_ = lastBase_ + need;
      *reinterpret_cast<size_t*>(buffer_ + lastBase_) = new_size;
      return ptr;
    }
    // Older block: copy to a fresh one (the old space is not reclaimed until reset()).
    size_t old_size = *reinterpret_cast<size_t*>(static_cast<uint8_t*>(ptr) - kAlign);
    void* fresh = allocate(new_size);
    if (fresh == nullptr) return nullptr;
    memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
    return fresh;
  }

 private:
  static const size_t kAlign = sizeof(void*) >= 8 ? 16 : 8;

  static size_t roundUp(size_t n) { return (n + (kAlign - 1)) & ~(kAlign - 1); }

  uint8_t* buffer_;
  size_t capacity_;
  size_t used_;
  void* lastData_;
  size_t lastBase_;
};

}  // namespace detail
}  // namespace parley

#endif  // PARLEY_DETAIL_FIXED_ARENA_H
