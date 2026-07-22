#ifndef PARLEY_TEST_ALLOC_COUNTER_H
#define PARLEY_TEST_ALLOC_COUNTER_H

// Global operator new/delete counters for the zero-heap-in-the-core-steady-state probe (plan §6). The
// core allocates nothing on any steady-state path (fixed arenas only); a test snapshots this counter
// around a core-only window (using a no-heap adapter + id generator) and asserts the delta is zero.
long parleyAllocCount();

#endif  // PARLEY_TEST_ALLOC_COUNTER_H
