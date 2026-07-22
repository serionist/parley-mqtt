# Parley — conventions

Stable engineering conventions, carried over from the parent project and adapted for a cross-platform
library. Build map: [`../CLAUDE.md`](../CLAUDE.md); the brief: [`../README.md`](../README.md).

## Cross-cutting (the working method)
- **Spec upstream of code.** Once the wire protocol is written, it's the contract; wire-behavior changes
  edit the spec + the conformance tests in the same change. Decisions get a dated entry in a decisions log
  with the rationale and the alternatives rejected.
- **Plan → review → implement → review.** `plan-first` (approved plan) → `review-plan` → implement →
  `review-code` (CLEAN) → close. No feature code on an unapproved plan.
- **Parallel APIs.** The C#/C++/JS public shapes stay aligned. Name things per each ecosystem's idiom, but
  the concepts and call sequences match; justify any forced divergence in the decisions log.
- **Thin edges, fat core.** All correlation/timeout/retransmit/dedup/heartbeat logic lives in each
  language's core. Transport adapters only translate publish/subscribe/setWill; generated stubs only add
  types.
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

## JS/TS
- TypeScript strict; ship ESM + CJS + `.d.ts`. Core has zero required runtime deps (bring your own MQTT
  client via a transport adapter).
- Promise-based; reject with a typed error for expected outcomes rather than throwing across the loop.
- Lint + typecheck clean; versions pinned in `package.json`.

## C++
- ESP32/Arduino-first; avoid exceptions/RTTI dependence; prefer static allocation and fixed-size pools.
- ArduinoJson for the codec; ALWAYS the explicit-length publish (never the null-terminated overload — it
  truncates PROGMEM/binary payloads).
- Pump from `loop()`; no blocking waits. Small per-call state; a native desktop build runs the tests
  without hardware.

## Tests
- A fake in-memory transport carries the bulk suite in every language (two endpoints talk with no broker).
- Language-neutral conformance vectors, replayed by all three implementations, are the anti-drift net.
- One real-broker integration test per language; one interop job across all three.

## Commits
- `feat(csharp): …`, `fix(protocol): …`, `docs: …`, `test: …`. Commit code + its spec/test updates
  together. Commit only when the user asks.
