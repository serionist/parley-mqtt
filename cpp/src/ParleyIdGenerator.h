#ifndef PARLEY_ID_GENERATOR_H
#define PARLEY_ID_GENERATOR_H

#include <stddef.h>
#include <stdint.h>

#include "detail/limits.h"

namespace parley {

// Injectable command-id source (protocol.md §4.2 — "The generator is injectable per implementation
// for constrained devices"). Tests inject a scripted generator; production overrides with a source of
// real entropy (esp_random on ESP32, etc.).
class ParleyIdGenerator {
 public:
  virtual ~ParleyIdGenerator() {}
  // Writes a fresh, §4.2-valid id (1–64 chars of [A-Za-z0-9_-], NUL-terminated) into `out` (capacity
  // `cap` >= kIdCapacity).
  virtual void next(char* out, size_t cap) = 0;
};

// A default generator producing 22-char base64url-ish ids (~128 bits) from a xorshift64 PRNG. The seed
// mixes a per-instance address and a monotone counter — adequate for the default; a device that needs
// cryptographic uniqueness across reboots supplies its own (§4.2). NOT for security decisions.
class ParleyDefaultIdGenerator : public ParleyIdGenerator {
 public:
  ParleyDefaultIdGenerator() {
    static uint64_t counter = 0x9E3779B97F4A7C15ULL;
    counter += 0x9E3779B97F4A7C15ULL;
    state_ = counter ^ reinterpret_cast<uintptr_t>(this) ^ 0xD1B54A32D192ED03ULL;
    if (state_ == 0) state_ = 0x1234567890ABCDEFULL;
  }

  void next(char* out, size_t cap) override {
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t n = 22;
    if (n + 1 > cap) n = cap - 1;
    for (size_t i = 0; i < n; ++i) {
      out[i] = kAlphabet[nextRandom() & 63];
    }
    out[n] = '\0';
  }

 private:
  uint64_t nextRandom() {
    uint64_t x = state_;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state_ = x;
    return x;
  }

  uint64_t state_;
};

}  // namespace parley

#endif  // PARLEY_ID_GENERATOR_H
