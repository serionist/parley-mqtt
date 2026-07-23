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

## 2026-07-22 — Conformance adjudications from vector authoring

Authoring the 40 vectors from the spec text surfaced six underdetermined points; adjudicated as:
**(1)** H1's immediate `hb` for a QUEUED/EXECUTING duplicate and **(2)** H7's stateless
`$cancelled` for an unknown-id cancel are promoted SHOULD → MUST — an exact-set conformance
runner cannot express optional output, and uniform behavior across four languages is the
project's point. **(3)** One heartbeat cadence per id, restarted by every `hb` publication.
**(4)** `resultRetryTimeout` is measured from the first `res` publication. **(5)** Vectors must
not schedule coincident timer expirations (implementations stay free on the tie-break);
construction-time subscribes are not asserted, only post-`connected` ones. **(6)** A `$busy`
received in SENDING is terminal under C4's general rule (the ignore exception is LIVE-scoped).
**Rejected:** an "optional output" vector construct (would make conformance ambiguous per
implementation); fixing a coincident-timer tie-break order (over-constrains implementations for
no interop gain).

## 2026-07-22 — Adjudications from the C# deep review

The six-lens review of the reference implementation settled five cross-language contracts:
**(1)** `$decodeError` — a sixth, commander-local-only builtin: a success result whose payload
fails typed deserialization is `rak`ed (redelivery cannot fix a contract mismatch) and surfaces
as `$decodeError`, not as a masqueraded `$executionStale`. Malformed progress payloads are
dropped silently (C8: advisory). **(2)** Custom errors are string codes: `TError` ↔ the wire `e`
string; `$`-prefix fail-fast; error `p` stays wire-legal but un-surfaced in v1. **(3)** A handler
outcome that fails to serialize is treated exactly like a thrown handler (abort: slot freed,
timers disposed, id tombstoned, commander degrades to `$executionStale`) — never a leak.
**(4)** Adapter `subscribe` has replace-semantics (one handler per topic). **(5)** Commander and
handler instances are disposable (detach subscriptions, complete in-flight calls locally with
`$cancelled`); the C# `ParleyOutcome` implicit-conversion token structs are a C#-local inference
idiom, not required of other languages.
**Rejected:** silent-drop of undecodable results (hides integration bugs behind stale timeouts);
surfacing structured error `p` in v1 (write side has no producer; revisit if demanded).

## 2026-07-22 — Adjudications from the TypeScript deep review

**(1) Publish comes last, in every language.** Synchronous-delivery adapters are legal, so all
state mutation and timer scheduling completes before `adapter.publish`; the TS review found
three inverted sites (one a permanent self-rearming timer leak on the cancel path) and the same
structure exists in C# — both are patched and the rule is now a design.md contract.
**(2) Thrown cancellation ≡ returned cancellation.** A handler unwinding via a cancellation
throw with its signal aborted yields `$cancelled` (matches C#'s `OperationCanceledException`
arm); only non-cancellation exceptions abort silently.
**(3) Conformance runners must not tolerate macrotask-deferred publishes**: the drain between
vector steps flushes microtasks only — zero real-timer boundaries.
**(4) TypeScript pinned at 6.0.3 (waiver).** Latest-stable is the native 7.x compiler, but
typescript-eslint's peer range (`<6.1.0`) and tsup's dts pipeline cannot consume it yet;
revisit at each `update-deps` pass.
**Rejected:** treating the reentrant C# lock as protection against synchronous re-entry (same
thread re-enters mid-mutation); runner-level tolerance of one macrotask hop (would let a
nonconformant dispatch pass).

## 2026-07-22 — C++ static-allocation adjudications

**(1)** An inbound message exceeding the fixed parse capacity (`MaxPayload`) is treated as
unparseable and silently dropped — a documented local extension of §4's malformed-input posture
to memory-limit overflow (identical observable: no parsed object). **(2)** The C++ adapter is
outbound-only; inbound/connected/tick live on the library objects (no stored closures).
**(3)** Commander pool exhaustion (`MaxInFlight` full) is a synchronous local failure, never a
`$` wire code. **(4)** `kNoTimeout` is a skip-scheduling branch (wraparound-safe), completing
the cap-disable parity set (C# `null` / TS `null` / Python `math.inf`).
**Rejected:** an error reply for oversize inbound (amplification, same as malformed input); a
`$`-code for local pool exhaustion (not a wire condition).

## 2026-07-22 — Adapter contract addition from the e2e matrix

**Decision:** adapters MUST fire the library's connected callback off the client's connect call
stack (defer via task/queue/next-tick). Found live: MQTTnet raises its connected event
synchronously inside `ConnectAsync`; the library's callback re-subscribes (calling back into
the client) under its dispatch gate → inline forwarding deadlocks and silently freezes every
timer. The e2e `broker-restart` scenario exposed it; design.md carries the contract, the C#
example adapter demonstrates the deferral.
**Also recorded:** QoS-1 redelivery-dedup over a real broker remains an open gap (needs
persistent sessions + induced disconnects); the e2e QoS-1 pass claims adapter compatibility
only.

## 2026-07-22 — Package identity, v1.0.0 lockstep, and OIDC-first publishing

**Decision:** publish all four libraries at **v1.0.0**, one lockstep version per release, from the
public repo `github.com/serionist/parley-mqtt`, under names that survive the taken bare `parley`:
**npm `parley-mqtt`**, **PyPI `parley-mqtt`** (import package stays `parley`), **NuGet `Parley.Mqtt`**
(assembly/namespace stay `Parley`), **PlatformIO `ParleyMqtt`**. MIT, © The Parley Authors. v1.0.0
is a semver + wire-stability commitment (protocol frozen at `v:1`; breaking changes = major bump).
Releases are cut by pushing a `v*` tag; a GitHub Actions workflow verifies the tag equals all five
manifest version fields (and that the tagged commit is an ancestor of `main`), runs the full gates
(four language suites + the docker e2e matrix), then publishes each package, and finally creates a
GitHub Release. Publishing uses **OIDC trusted publishing wherever the registry supports it**, so
no long-lived registry secret is stored — an explicit, narrow exception to the no-secrets
convention, which governs *broker* credentials, not CI publish identity.

**Per-registry reality (verified against current official docs at implementation):**
- **npm** has **no pending-publisher** mechanism — the very first publish of a new name cannot be
  tokenless. Bootstrap (one-time, [`docs/publishing.md`](publishing.md)): a temporary granular
  automation token as a repo secret → the CI job publishes 1.0.0 with `--provenance` (provenance
  works with a token when `id-token: write` is present) → configure the npm Trusted Publisher →
  delete the token. The workflow's npm job is **dual-path**: it uses `NODE_AUTH_TOKEN` if the secret
  is present, otherwise tokenless OIDC; `id-token: write` is always set. Trusted publishing needs
  **npm ≥ 11.5.1 / Node ≥ 22.14**, so the job upgrades npm before publishing.
- **PyPI** supports a **pending publisher** that creates the project on first use; the runbook lists
  the exact fields (project name + owner/repo + workflow filename + environment) and the
  **squat-invalidation caveat** (if someone registers `parley-mqtt` first, the pending publisher is
  invalidated). Publish via `pypa/gh-action-pypi-publish`.
- **NuGet** uses an **owner-scoped** trusted-publishing policy (no package pre-existence needed):
  `NuGet/login` (user = nuget.org **username**, not email) exchanges the OIDC token for a 1-hour API
  key → `dotnet nuget push`. Gradual rollout is in progress; the runbook documents an API-key
  fallback.
- **PlatformIO** has no OIDC: a `PLATFORMIO_AUTH_TOKEN` repo secret drives `pio pkg publish` (which
  packs `cpp/` from the subdir). Documented exception to no-secrets, same rationale as above.

**PEP 639:** Python metadata uses the SPDX `license = "MIT"` expression + `license-files`, and the
deprecated `License :: OSI Approved :: MIT License` classifier is **dropped** — the two are mutually
exclusive under Metadata 2.4 and modern tooling rejects the combination. Status classifier moves to
`5 - Production/Stable`.

**Arduino Library Manager deferred:** the Library Manager requires the library at the **root** of a
registered repo; a monorepo subdir does not qualify. v1 ships to the PlatformIO Registry (which
packs from `cpp/`); an automated single-library mirror repo (`serionist/parley-mqtt-cpp`) for the
Arduino LM is a **follow-up**, not built now.

**Supply chain:** every third-party GitHub Action is **SHA-pinned** to a resolved commit (annotated
tags resolved to the underlying commit), kept current by Dependabot; C# packages carry SourceLink +
a `snupkg` symbol package built deterministically in CI. Packaged, registry-rendered READMEs (with
absolute repo links) ship per ecosystem so the root `README.md` keeps its relative links for GitHub.
**Rejected:** a single shared package name (bare `parley` unavailable); storing long-lived registry
API keys as the primary path (OIDC preferred); rewriting the root README's links to absolute for all
registries (per-package READMEs are cleaner and keep GitHub rendering intact).
