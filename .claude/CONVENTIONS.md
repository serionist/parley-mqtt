# Parley — conventions

Stable engineering conventions, carried over from the parent project and adapted for a cross-platform
library. Build map: [`../CLAUDE.md`](../CLAUDE.md); the brief: [`../README.md`](../README.md).

## Cross-cutting (the working method)
- **Spec upstream of code.** The wire protocol (`docs/protocol.md`) is the contract; wire-behavior
  changes edit the spec + the conformance tests in the same change. Decisions get a dated entry in
  `docs/decisions.md` with the rationale and the alternatives rejected.
- **Plan → review → implement → review.** `plan-first` (approved plan) → `review-plan` → implement →
  `review-code` (CLEAN) → close. No feature code on an unapproved plan.
- **Parallel APIs.** The C#/Python/C++/JS public shapes stay aligned. Name things per each ecosystem's
  idiom, but the concepts and call sequences match; justify any forced divergence in the decisions log.
- **Thin edges, fat core.** All correlation/timeout/retransmit/dedup/heartbeat logic lives in each
  language's core. Transport adapters only translate publish/subscribe/unsubscribe + the connected
  callback; codecs only translate typed payloads ↔ envelope JSON.
- **Bounded state, always.** No indefinitely growing collection anywhere: every table (in-flight,
  dedup, result cache, tombstones, timers) is TTL'd or hard-capped with explicit eviction. Each
  language's suite includes sustained-load steady-state tests proving it (`docs/testing.md`) —
  the targets are long-running processes on very small hardware, the JS build especially.
- **Latest-stable, pinned.** Every dependency + toolchain pinned to a concrete newest-stable version
  (no floating ranges, no `:latest`, no prereleases without a recorded waiver). Kept current via
  `update-deps`.
- **Context7 first** for any third-party library API before coding against it — APIs drift.
- **No secrets, no consumer specifics.** Public repo. Broker credentials/TLS are the consumer's, supplied
  at runtime; placeholder namespaces + endpoint ids in all examples/tests.
- **Section edits over rewrites.** The diff is the review. Small, verifiable steps; foundations before
  surface.
- **Gates are the review authority.** Each language builds warnings-clean, its formatter verifies
  no-changes, its test suite passes — before anything is called done.

## C#
- Multi-target to embed broadly (e.g. `netstandard2.0` + a current `net*`); `Nullable` enabled;
  warnings-as-errors; XML docs on public API.
- `System.Text.Json` source generators for serialization (AOT/trim-safe) — no reflection serializers.
- `async`/`Task`; thread `CancellationToken`; never `async void`.
- Core has no concrete-MQTT-client reference; the MQTT transport lives in a separate adapter package.
- Central package pins (`Directory.Packages.props`); format with `dotnet format`.

## Python
- asyncio-first; no threads in the core. Fully type-hinted, `py.typed` shipped; current CPython versions
  supported, pinned dev toolchain.
- Zero required runtime deps in core (bring your own MQTT client via a transport adapter; a paho-mqtt
  reference adapter ships separately).
- Expected outcomes return as values (`ParleyResult`), never raised; wire-level cancel maps to task
  cancellation (`asyncio.CancelledError`).
- `pytest` for tests; `ruff` format + lint clean; versions pinned.

## JS/TS
- TypeScript strict; ship ESM + CJS + `.d.ts`. Core has zero required runtime deps (bring your own MQTT
  client via a transport adapter).
- Promise-based; expected outcomes resolve as values (a `ParleyResult` discriminated union), never as
  rejections — throws/rejections are reserved for programming errors.
- Lint + typecheck clean; versions pinned in `package.json`.

## C++
- ESP32/Arduino-first; avoid exceptions/RTTI dependence; prefer static allocation and fixed-size pools.
- ArduinoJson for the codec; ALWAYS the explicit-length publish (never the null-terminated overload — it
  truncates PROGMEM/binary payloads).
- Pump from `loop()`; no blocking waits. Small per-call state; a native desktop build runs the tests
  without hardware.

## Tests
- A fake in-memory transport carries the bulk suite in every language (two endpoints talk with no broker).
- Language-neutral conformance vectors (`docs/conformance/`, format in `docs/protocol.md` §10), replayed
  by all four implementations over a virtual clock, are the anti-drift net.
- Sustained-load bounded-state tests per language: thousands of command cycles (incl. failure/
  timeout/cancel paths) under the virtual clock, then assert every internal collection is back at
  steady-state size.
- The dockerized e2e matrix (`docs/testing.md`) runs all commander→handler language pairs against
  a real broker (mosquitto); its diagonal (X→X) subsumes the per-language real-broker integration
  test once the matrix exists.

## Commits
- `feat(csharp): …`, `fix(protocol): …`, `docs: …`, `test: …`. Commit code + its spec/test updates
  together. Commit only when the user asks.
