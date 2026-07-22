# Parley — decisions log

Dated, append-only. Each entry: the decision, the rationale, and the alternatives rejected.
Newest at the bottom. The normative contract lives in [`protocol.md`](protocol.md); this file
records *why* it looks the way it does.

## 2026-07-22 — Four target languages

**Decision:** C#, Python, JS (TypeScript types), C++ (ESP32/Arduino-first) — four parallel
reference implementations of one wire protocol, with deliberately matching public shapes
(`ParleyCommander` / `ParleyCommandHandler` / adapter / config).
**Rejected:** the original three-language plan (no Python).

## 2026-07-22 — Command-centric, consumer-supplied topics

**Decision:** one Commander/Handler pair per command type; four opaque topic strings (request,
heartbeat, progress, result) supplied by the consumer; no namespace or naming convention in the
library. MQTT is the method dispatcher. Multiple commanders may share a topic set (own-id filter);
exactly one active handler per topic set.
**Why:** thinner library, no routing layer in the payload, and topic ACLs stay fully in the
consumer's hands — per-caller segmentation is a provisioning choice, not a protocol feature.
**Rejected:** the endpoint-centric fixed scheme (`<ns>/rpc/req/<callee>`, one reply inbox per
caller, method name in the payload).

## 2026-07-22 — Thin transport adapter; the library never owns the connection

**Decision:** an abstract adapter per language (publish / subscribe / unsubscribe / connected
callback) that the consumer implements over their own MQTT client. The core has no MQTT package
dependency. The library re-subscribes on the adapter's connected signal.
**Why:** MQTT-version agnosticism (3.1.1+), no dependency drag, consumer keeps credentials/TLS.
**Rejected:** bundling concrete clients; requiring auto-resubscribing "managed" clients.

## 2026-07-22 — No endpoint presence (retained heartbeat + Last-Will) in scope

**Decision:** Parley has no per-endpoint presence feature; the heartbeat is per-execution only.
**Why:** a Last-Will must be registered at connect time — structurally impossible for a library
that is handed an already-live connection. Consumers who want presence own the connection and can
implement it themselves.
**Rejected:** the brief's library-managed retained `hb/<endpoint>` + LWT.

## 2026-07-22 — First heartbeat is the accept-ack; sign-of-life model

**Decision:** no dedicated ack message. The handler heartbeats each admitted command id from
**admission** (received/queued, not execution start); the commander stops request retries on the
first sign of life (heartbeat, progress, or result) and judges liveness by a sliding
sign-of-life window from then on. `$noAck` therefore always means "unreachable", never "busy".
**Why:** one message kind fewer, four topics suffice, and queued commands on a serial handler
still show life; a single uniform liveness rule replaces ack + progress-stall special cases.
**Rejected:** a dedicated ack message; heartbeat starting at execution start; heartbeat batching
(one message listing all active ids — saves little at this scale, complicates the envelope).

## 2026-07-22 — Results are acked and retried

**Decision:** the handler re-publishes the result until the commander's `rak` arrives or its
retry budget expires; both sides keep tombstones so late duplicates are re-acked/ignored, never
re-processed.
**Why:** the brief's "redelivered until acknowledged" had no actual mechanism (redelivery only
piggybacked on request retries, which stop at ack); this closes the hole symmetrically.
**Rejected:** implicit redelivery via request re-sends; broker-level QoS 2.

## 2026-07-22 — No wire-carried expiries; retry budgets only

**Decision:** no absolute timestamps on the wire. All give-up decisions are caller-local timers
(`noAckTimeout`, `staleTimeout`, `executionTimeout`) and handler-local budgets
(`resultRetryTimeout`).
**Why:** embedded targets have no reliable clock; expiry fields invite cross-device clock-skew
bugs. The residual "broker-queued request executes after the caller gave up" case is documented
as inherent (protocol.md §9) rather than half-fixed.
**Rejected:** an absolute expiry field in the request.

## 2026-07-22 — Two timeout mechanisms, five builtin error codes

**Decision:** (a) a sliding sign-of-life window — expiry before any sign of life → `$noAck`,
after → `$executionStale`; (b) an overall execution cap from first sign of life →
`$executionTimeout`. Builtin codes: `$noAck`, `$executionStale`, `$executionTimeout`,
`$cancelled`, `$busy`; `$`-prefix reserved, custom codes are a generic type parameter.
**Why:** a single sliding timer cannot express "accepted but never finished" — a forever-
heartbeating job would never trip it. Distinct codes keep "peer gone" apart from "job slow" apart
from "handler full".
**Rejected:** one-timer-three-codes; blunt single timeout.

## 2026-07-22 — Bounded handler state; stateless `$busy` on overflow

**Decision:** fixed per-command state pools are legal (ESP32); a full handler answers each
request for an unknown id with a stateless `$busy` error result (not cached, no `rak`, re-emitted
per duplicate). Dedup lookup precedes the capacity check, so duplicates of admitted ids never see
`$busy`.
**Why:** the caller can tell "full" from "dead" and fails fast; statelessness costs the handler
nothing when it can least afford state.
**Rejected:** silent-drop-on-overflow (caller degrades to a slow `$noAck` and cannot distinguish
full from dead).

## 2026-07-22 — Wire-level cancel

**Decision:** a `cnl` message on the request topic, re-sent until the caller reaches any terminal
outcome; the handler surfaces it as the language's native cancellation
(`CancellationToken` / task cancellation / AbortSignal / poll flag) and replies with a
`$cancelled` result (stateless if it never knew the id).
**Why:** long-running commands (backups, updates) need a way to actually stop work, not just be
abandoned locally.
**Rejected:** local-only abandon in v1.

## 2026-07-22 — Serial handler by default

**Decision:** `maxConcurrentExecutions` defaults to 1 everywhere; larger runtimes may raise it.
**Why:** matches the constrained target's reality and makes the default behavior identical across
all four languages; queued commands still heartbeat, so serial execution is never mistaken for
absence.

## 2026-07-22 — Per-call options object, not config override

**Decision:** `Execute` takes a small per-call options object (commander-local timeouts, progress
sink); connection-ish and handler-side settings stay constructor-only. C# uses `IProgress<T>`;
Python/JS take callbacks; C++ takes function pointers + context.
**Rejected:** overriding the whole `ParleyConfig` per call.

## 2026-07-22 — No codegen in v1; native generics

**Decision:** typing comes from each language's own system — C# generics, C++ templates, TS
generics, Python type hints. No schema compiler, no generated stubs. Cross-language agreement on
payload shapes is the consumer's contract, enforced by the interop test job.
**Why:** an entire toolchain less to build/version; the library stays a pure protocol engine.
**Rejected:** the brief's schema-driven codegen (may return later as an optional layer on top).

## 2026-07-22 — QoS-agnostic, correct at QoS 0; nothing retained

**Decision:** the protocol's own retry + dedup provides reliability; adapters may use QoS 0 or 1;
QoS 2 never required; retain is never used (the brief's retained "last result" slot is dropped).
**Why:** works identically on every broker/client combination from MQTT 3.1.1 up; retained slots
are trivially done by the consumer if wanted.

## 2026-07-22 — Cancellation stops request retries

**Decision:** cancelling a command that is still in SENDING stops the `req` re-sends; only `cnl`
keeps being re-sent (protocol.md C7). The fast terminal path is the handler's stateless
`$cancelled` for an unknown id; the `noAckTimeout` bounds the exchange either way.
**Why:** a command the caller no longer wants must not keep being offered for admission — cancel
should minimize the chance of execution, not race it.
**Rejected:** continuing `req` retries during cancellation so the command is admitted and then
cancelled on the handler.

## 2026-07-22 — Envelope: compact JSON, `v` field, `$`-reserved codes

**Decision:** single-object UTF-8 JSON envelope with short keys (`v`, `t`, `id`, `p`, `ok`, `e`,
`m`); every message versioned with `v:1`; unknown version/type/fields ignored; error codes
starting with `$` reserved for the protocol.
**Why:** tiny-buffer friendly, MQTT-3.1.1-safe (no user properties needed), forward-compatible.

## 2026-07-22 — Malformed input is silently ignored

**Decision:** receivers silently ignore payloads that fail to parse, messages missing required or
conditionally required fields, and ids violating §4.2 — extending §4's existing ignore-list
(unknown `v`/`t`, wrong topic, unknown fields).
**Why:** one uniform receiver posture; an error reply to garbage invites reply loops and gives an
attacker/misconfigured peer a traffic amplifier; four implementations must not diverge on this.
**Rejected:** error replies to malformed input; treating a missing `v` as v1.

## 2026-07-22 — `maxActiveCommands`: a uniform, portable admission cap

**Decision:** §8 gains `maxActiveCommands` (handler, default 32) — the cap H2 enforces; a slot is
held from admission through RESULT-CACHED and freed at the tombstone transition. C++ supplies it
via the pool template parameter.
**Why:** completes the Busy design and makes the `$busy` conformance vectors portable — an
implementation with uncappable pools could never replay them.
**Rejected:** leaving capacity entirely implementation-defined (untestable across languages).

## 2026-07-22 — Conformance vector format v1 (extended)

**Decision:** §10's format is fully specified before any vector is authored: call-start/cancel
steps, scripted ids, connection-event step + subscribe expectations, raw-payload negatives,
handler-stub actions (time-scripted, non-reactive), per-call `outcomes`, same-instant determinism
rules, and per-vector `rules` clause annotations. Vectors are written from the spec text before
the first implementation exists.
**Why:** the vectors are the anti-drift net; if they were shaped by the first implementation, a
misreading would go green and every later language would inherit it.
**Rejected:** implementation-specific test scripts; auditing only vector *coverage* rather than
per-vector correctness.

## 2026-07-22 — Bounded state everywhere (user requirement)

**Decision:** every internal collection in every implementation — in-flight tables, dedup, result
caches, tombstones (both sides), timer registrations — is TTL'd or hard-capped with explicit
eviction; per-language suites include sustained-load steady-state tests proving it.
**Why:** the targets are long-running processes on very small hardware (the JS build especially:
Shelly-class edge engines); a slow leak is a field failure.
**Rejected:** relying on GC/idle churn; unbounded caches with "practically never grows" arguments.
