import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    // Real timers only: the library is driven by an injected virtual clock, and the vector runner
    // flushes the microtask queue only (zero real-timer boundaries — TQ1). Faking global timers is
    // unnecessary and would only add confusion. See test/support/virtualClock.ts and support/drain.ts.
    environment: 'node',
    include: ['test/**/*.test.ts'],
    globals: false,
  },
});
