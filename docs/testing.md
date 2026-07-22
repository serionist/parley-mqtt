# Parley — testing strategy

**Status: normative for test infrastructure.** The wire contract itself is
[`protocol.md`](protocol.md); this document fixes how conformance to it is proven, in three
layers, and defines the language-neutral artifacts (conformance vectors, the test-peer contract)
that every implementation shares.

## The three layers

| Layer | Scope | Transport | Clock | Authored |
|---|---|---|---|---|
| 1. Unit tests | one language's internals | fake in-memory (injected by the test project as the adapter) | virtual | per language |
| 2. Conformance vectors | wire behavior of the protocol core | fake, scripted by the runner | virtual | **once**, language-neutral |
| 3. E2E matrix | real-world interop | real broker (dockerized mosquitto) | real | suite once + one thin peer per language |

Layer 2 is the workhorse and the anti-drift net: if two implementations both pass the vectors,
matrix failures can only come from real-world residue (codec quirks, client behavior, QoS,
reconnects, timing) — which is exactly what layer 3 exists to catch. Layer 1 covers what the wire
cannot see.

## Layer 1 — unit tests (per language)

The test project constructs the library with a **fake in-memory transport** as its adapter and a
**virtual clock**. Required coverage beyond the ordinary:

- **Atomic admission** — concurrent duplicate delivery must not double-admit (dedup lookup +
  capacity check are one atomic step, protocol.md H1/H2).
- **Pool behavior** — `maxActiveCommands` overflow → stateless `$busy`; tombstone TTL and
  oldest-first pool eviction on both sides.
- **Bounded state (hard requirement)** — sustained load: thousands of command cycles including
  failure, timeout, `$busy`, and cancel paths under the virtual clock; afterwards assert every
  internal collection (in-flight, dedup, result cache, tombstones, timer registrations) has
  returned to steady-state size. No implementation ships without this test.
- **Adapter wiring** — the connected signal re-subscribes exactly the library's topics.

## Layer 2 — conformance vectors

Vectors live in [`conformance/`](conformance/) as JSON files in the protocol.md §10 format (step
types, determinism rules, per-vector `rules` annotations). They are written **from the spec
text** and never adjusted to match an implementation — a vector/implementation disagreement is
resolved by re-reading protocol.md, and if the spec is wrong, the spec + vector change together.

Every language ships a **vector runner** in its test suite:

- drives the protocol core through the fake transport and virtual clock;
- supports both roles (`commander` / `handler`), all step directives, and exact-set `expect`
  matching (JSON equality up to object key order; no extra, no missing outbound actions);
- bypasses the id generator with the scripted `call.id` values;
- replays **every** file in `conformance/` — no skips without a recorded waiver.

## Layer 3 — the e2e matrix

### The test-peer executable

Each language ships one small `parley-testpeer` executable (docker image), implementing both
roles over its real MQTT adapter:

```
parley-testpeer --role handler   --broker <url> --prefix <topic-prefix>
parley-testpeer --role commander --broker <url> --prefix <topic-prefix> --report <path>
```

- **Handler role:** registers handlers for the standard test command types under
  `<topic-prefix>/<command>/{req,hb,prg,res}` and serves until stopped.
- **Commander role:** runs the standard interop suite against whatever handler is on the other
  side and writes a JSON report (`{ scenario, pass, detail }` per entry, nonzero exit on any
  failure).

### Standard test command types (language-neutral)

| Command | Handler behavior | Exercises |
|---|---|---|
| `echo` | returns the request payload immediately | happy path, codec fidelity (unicode, numbers, nesting) |
| `slow` | runs for a scripted duration, emitting N progress updates | heartbeats, progress, execution cap |
| `err` | fails with a scripted custom error code | error results, custom code typing |
| `cancelme` | runs until cancelled, then confirms | wire-level cancel |
| `busy1` | configured with `maxActiveCommands: 1` and a slow first call | stateless `$busy` |

The interop suite calls each command with agreed payloads and asserts outcomes, progress counts,
and error codes. Scenario definitions live beside the vectors in `conformance/interop/` (one JSON
file: scenarios, payloads, expected outcomes) so peers implement the harness, never the suite.

### The matrix driver

A docker-compose setup: one mosquitto broker + the test-peer images. The driver runs **every
ordered language pair** (commander × handler — 16 with four languages), each pair on an isolated
topic prefix, and aggregates the reports. The diagonal (X→X) doubles as each language's
real-broker integration test. Adding a language = adding its peer image; the matrix grows by
itself.

## Gates

A change is done when: the language builds warnings-clean, its formatter verifies no-changes, its
unit suite passes, its vector runner replays all of `conformance/` green — and, once the matrix
exists, the full matrix passes in CI.
