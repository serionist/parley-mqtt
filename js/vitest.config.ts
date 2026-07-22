import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    // Real timers only: the library is driven by an injected virtual clock, and the vector
    // runner drains microtasks over real macrotask boundaries. Faking global timers would break
    // the drain. See test/support/virtualClock.ts and test/support/drain.ts.
    environment: 'node',
    include: ['test/**/*.test.ts'],
    globals: false,
  },
});
