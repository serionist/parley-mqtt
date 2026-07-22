/**
 * Flushes the microtask queue to quiescence. The library defers work only through the injected
 * virtual clock (macrotasks that never fire during a drain) and through `await` continuations
 * (microtasks). A real `setTimeout(0)` boundary drains the entire pending microtask queue before
 * its callback runs; a few boundaries give ample margin for the library's shallow await chains.
 *
 * Draining advances no virtual time (the virtual clock is independent of the real timer used here),
 * so the §10 timer-determinism equivalence is preserved.
 */
export async function drainMicrotasks(): Promise<void> {
  for (let i = 0; i < 4; i++) {
    await new Promise<void>((resolve) => {
      setTimeout(resolve, 0);
    });
  }
}
