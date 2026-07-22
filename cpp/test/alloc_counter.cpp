#include "alloc_counter.h"

#include <cstdlib>
#include <new>

// Global allocation counter. operator new is routed through malloc and counted; a zero delta over a
// core-only window proves the steady-state heap-free property. Built with -fno-exceptions, so these
// overloads never throw — a null return on OOM is acceptable in a host test binary.
static long g_alloc_count = 0;

long parleyAllocCount() { return g_alloc_count; }

void* operator new(std::size_t n) {
  ++g_alloc_count;
  return std::malloc(n == 0 ? 1 : n);
}

void* operator new[](std::size_t n) {
  ++g_alloc_count;
  return std::malloc(n == 0 ? 1 : n);
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
