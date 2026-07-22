# Parley wire protocol — v1

**Status: normative.** This document is the contract: implementations serve it, and any change to
wire behavior edits this file (and the conformance vectors) in the same change. The
[`README.md`](../README.md) brief and [`design.md`](design.md) are non-normative companions;
rationale and rejected alternatives live in [`decisions.md`](decisions.md).

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are to be interpreted as described in
RFC 2119.

## 1. Model and terms

- **Command type** — a single RPC operation. It is identified by its **topic set** (§3), not by a
  method name in the payload; MQTT itself is the dispatcher.
- **Commander** — the caller role for one command type. **Handler** — the callee role. One process
  may hold many commanders and handlers, for many command types, over one shared connection.
- **Command** — one invocation of a command type, identified by a **command id** (§4.2).
- **Sign of life** — any `hb`, `prg`, or `res` message received by a commander for a given command
  id (§6).

Multiple commander instances MAY share a command type's topic set concurrently (§6, rule C6).
**A topic set MUST NOT have more than one active handler** — concurrent handlers would race
admission and break at-most-once; distributing a command type over several workers is out of scope
for v1.

## 2. Transport assumptions

- Any MQTT **3.1.1 or later** broker. No MQTT 5 features are used: correlation, message typing, and
  versioning all live in the payload.
- The library is handed a **live, connected session** through a transport adapter. It never
  connects, reconnects, authenticates, or configures TLS, and it never registers a Last-Will.
  Security is entirely the broker's concern (ACLs + TLS on the consumer's connection).
- **Correct at QoS 0.** All reliability is provided by the protocol's own retries and
  deduplication. Adapters MAY publish/subscribe at QoS 0 or 1; QoS 1 duplicates are absorbed by
  dedup for as long as the id's dedup/tombstone state is retained (§9, case 5). QoS 2 MUST NOT be
  required by any implementation.
- Messages MUST be published with **retain = false**. No parley topic ever holds retained state.
- After a reconnect, subscriptions MUST be re-established: the adapter signals
  connection-established, and the library re-subscribes every topic it uses. In-flight timers keep
  running across the gap (a long disconnect legitimately ends calls with a timeout error).

## 3. Topics

A command type is provisioned with **four consumer-supplied topic strings**. Parley imposes no
naming scheme, no namespace, and no structure on them — they are opaque. They MUST be distinct
within the topic set and MUST NOT contain MQTT wildcards.

| Topic (config name) | Direction | Message types carried |
|---|---|---|
| `request` | commander → handler | `req`, `rak`, `cnl` |
| `heartbeat` | handler → commander | `hb` |
| `progress` | handler → commander | `prg` |
| `result` | handler → commander | `res` |

The handler subscribes to `request`; each commander subscribes to `heartbeat`, `progress`, and
`result`. All commanders of a command type share the three handler→commander topics and filter by
command id (rule C6).

## 4. Message envelope

Every message is a single **UTF-8 JSON object**. Common fields:

| Field | Type | Presence | Meaning |
|---|---|---|---|
| `v` | integer | REQUIRED | protocol version; this document defines `1` |
| `t` | string | REQUIRED | message type: `req` `hb` `prg` `res` `rak` `cnl` |
| `id` | string | REQUIRED | command id (§4.2) |

Receivers MUST silently ignore: payloads that fail to parse as a JSON object; messages lacking a
required field — including conditionally required ones (`ok` on `res`; `e` when `ok:false`);
messages whose `id` violates §4.2; messages with an unrecognized `v` or `t`; message types
arriving on the wrong topic; and unknown additional fields (forward compatibility). `v` increments
only on breaking changes to the envelope or the normative behavior.

### 4.1 Message reference

```jsonc
{"v":1, "t":"req", "id":"kD3fx9Qw", "p": { /* request params */ }}        // request
{"v":1, "t":"hb",  "id":"kD3fx9Qw"}                                        // heartbeat
{"v":1, "t":"prg", "id":"kD3fx9Qw", "p": { /* progress payload */ }}       // progress
{"v":1, "t":"res", "id":"kD3fx9Qw", "ok":true,  "p": { /* result */ }}     // result: success
{"v":1, "t":"res", "id":"kD3fx9Qw", "ok":false, "e":"$busy", "m":"..."}    // result: error
{"v":1, "t":"rak", "id":"kD3fx9Qw"}                                        // result-ack
{"v":1, "t":"cnl", "id":"kD3fx9Qw"}                                        // cancel
```

- `p` — any JSON value; omitted/`null` means "no payload". Present on `req`, `prg`, and successful
  `res`.
- `res` additionally carries `ok` (boolean, REQUIRED). When `ok:false`: `e` (error code string,
  REQUIRED) and `m` (human-readable detail, OPTIONAL). `p` MAY accompany an error for structured
  error data.
- Retransmissions of a message MUST be byte-for-byte re-publishes of the same logical message
  (same `id`, same content).

### 4.2 Command ids

An opaque string, 1–64 characters from `[A-Za-z0-9_-]`, chosen by the commander. Ids MUST be
unique per topic set for as long as any peer might hold state for them (in-flight, result cache,
or tombstone). In practice: at least 64 bits of randomness; a UUID is fine where cheap. The
generator is injectable per implementation for constrained devices.

## 5. Error codes

Error codes are strings. Codes beginning with `$` are **reserved for the protocol**; consumer
(custom) codes MUST NOT begin with `$`. The five builtin codes:

| Code | Origin | Meaning |
|---|---|---|
| `$noAck` | commander-local | sign-of-life window expired before *any* sign of life — handler unreachable (never "busy", see `$busy`) |
| `$executionStale` | commander-local | sign-of-life window expired *after* signs of life — handler went silent mid-command |
| `$executionTimeout` | commander-local | overall execution cap expired — handler alive but the command never finished |
| `$cancelled` | wire (`res`) | the command was cancelled before producing a result |
| `$busy` | wire (`res`) | the handler refused admission: its per-command state pool is full |

Custom codes flow only inside error `res` messages (`e` field) and are surfaced to the consumer
typed (the error-code generic parameter in each language binding).

## 6. Commander behavior

Per command, a commander is in one of: **SENDING** → **LIVE** → terminal (**SUCCEEDED** /
**FAILED**), plus a post-terminal **tombstone** record kept for `tombstoneTtl`.

- **C1 — send + retry.** On execute: generate the id, publish `req`, and re-publish it every
  `requestRetryInterval` while SENDING.
- **C2 — first sign of life.** On the first `hb`/`prg`/`res` for the id: enter LIVE, stop `req`
  retries, start the execution cap (`executionTimeout`, measured from this moment). The
  sign-of-life timer is **sliding**: it restarts on every sign of life.
- **C3 — timeouts.** Sign-of-life timer expiry in SENDING → terminal `$noAck`. Expiry in LIVE →
  terminal `$executionStale`. Execution cap expiry in LIVE → terminal `$executionTimeout`. The
  pre-first-sign-of-life window (`noAckTimeout`) and the sliding window (`staleTimeout`) are
  separately configurable durations of the same timer mechanism.
- **C4 — result.** A `res` for a non-terminal id is terminal: `ok:true` → SUCCEEDED with `p`;
  `ok:false` → FAILED with `e`/`m`/`p`. The commander MUST publish `rak` — except for `e:"$busy"`,
  for which `rak` MUST NOT be sent (the handler holds no state for a refused id). One exception:
  a `$busy` `res` received in LIVE MUST be ignored — the handler sends `$busy` only for unadmitted
  ids and a sign of life proves admission, so such a message can only be a stale, reordered
  pre-admission reply.
- **C5 — tombstone.** On *any* terminal outcome the id enters the tombstone table for
  `tombstoneTtl`. A `res` received for a tombstoned id → re-publish `rak` (unless `e:"$busy"`) and
  discard the content. This stops a handler's result retries even when the commander already gave
  up or completed. The tombstone table MAY additionally be hard-capped (oldest-first eviction) —
  all commander-side state is bounded.
- **C6 — own-id filter.** Any inbound message whose id is neither in-flight nor tombstoned MUST be
  ignored silently — it belongs to another commander instance. A commander MUST NOT `rak` a result
  it did not originate.
- **C7 — cancel.** On local cancellation of a non-terminal command: publish `cnl` immediately,
  then every `cancelRetryInterval`, until the command reaches *any* terminal outcome — a `res`
  (typically `$cancelled`, possibly a genuine result that won the race) or a C3 timeout. C3 timers
  keep running during cancellation, so cancel re-sending is always bounded. Cancellation in
  SENDING also stops the C1 `req` retries: a command the caller no longer wants MUST NOT keep
  being offered for admission — the fast terminal path is then the handler's stateless
  `$cancelled` (H7).
- **C8 — progress.** `prg` payloads for an in-flight id are delivered to the consumer's progress
  callback. Delivery is advisory: loss, duplication, and reordering MUST be tolerated. `prg` is
  never acknowledged.

## 7. Handler behavior

Per admitted command, a handler tracks: **QUEUED** → **EXECUTING** → **RESULT-CACHED** →
**tombstone** (for `tombstoneTtl`) → evicted. The dedup table spans all four stages.

- **H1 — dedup before capacity.** On `req`, look up the id in the dedup table **first**; the
  capacity check applies only to unknown ids. For a duplicate of an admitted id: QUEUED/EXECUTING
  → SHOULD publish an immediate `hb` (resynchronizes the commander cheaply); RESULT-CACHED →
  re-publish the cached `res` immediately; tombstoned → ignore. A duplicate of an admitted id
  MUST NOT receive `$busy`, even while the handler is full for new ids.
- **H2 — admission / Busy.** For an unknown id with a free command slot (`maxActiveCommands`,
  §8): admit — create the dedup entry, enqueue, and publish the first `hb` **immediately** (this
  heartbeat is the accept-ack). With no free slot: publish a **stateless** error `res` with
  `e:"$busy"` — no state
  is created, no result is cached, no `rak` is expected — and re-emit it for each further `req`
  of that id while full. A lost `$busy` self-heals through the commander's request retries.
- **H3 — heartbeats from admission.** From admission until the command's first `res` is
  published, publish `hb` for the id every `heartbeatInterval`. Queued commands heartbeat exactly
  like executing ones — a caller can never conclude "unreachable" from "busy".
- **H4 — execution.** Dequeue FIFO into at most `maxConcurrentExecutions` slots (default 1 —
  serial). The consumer's handler receives the request payload, a progress emitter (each call
  publishes one `prg`), and a cancellation signal.
- **H5 — result retry.** On completion publish `res`, cache it, and re-publish every
  `resultRetryInterval` until a `rak` for the id arrives or `resultRetryTimeout` expires; either
  way the id then moves to the tombstone stage and is evicted after `tombstoneTtl`.
- **H6 — unknown acks.** A `rak` for an id not in RESULT-CACHED MUST be ignored.
- **H7 — cancel.** On `cnl`: EXECUTING → trigger the cancellation signal; the eventual outcome is
  whatever the handler produces (an aborted handler yields an error `res` with `e:"$cancelled"`).
  QUEUED → remove from the queue and publish an error `res` `$cancelled` (cached and retried per
  H5). RESULT-CACHED → re-publish the cached `res` (cancel lost the race). Unknown or tombstoned
  id → SHOULD publish a **stateless** error `res` `$cancelled` (mirror of `$busy`: no state, no
  `rak` expected — it only exists to terminate the caller quickly).

  *Note:* rule C4 has the commander `rak` a stateless H7 `$cancelled`; the handler ignores it per
  H6. Only `$busy` is exempt from `rak`.
- **H8 — capacity.** Fixed pools are legal and expected on constrained targets; `maxActiveCommands`
  (§8) is the uniform admission cap, and a slot is held from admission through RESULT-CACHED,
  freed at the tombstone transition. The H2 admission check MUST reserve the command's full
  lifecycle through RESULT-CACHED — an admitted command can always execute and cache its result;
  only the tombstone pool may be sized independently of admissions. When the tombstone pool
  overflows, the oldest entries are evicted first.

## 8. Timing configuration

All values are per-deployment configuration; Parley does not negotiate them over the wire.
Defaults (all overridable; commander-local values also per-call):

| Parameter | Side | Default | Notes |
|---|---|---|---|
| `requestRetryInterval` | commander | 2 s | C1 |
| `noAckTimeout` | commander | 10 s | C3; SHOULD be ≥ 3–5× `requestRetryInterval` |
| `staleTimeout` | commander | 15 s | C3; SHOULD be ≥ 3× the handler's `heartbeatInterval` |
| `executionTimeout` | commander | 60 s | C2/C3; MAY be disabled (no cap) |
| `cancelRetryInterval` | commander | 2 s | C7 |
| `heartbeatInterval` | handler | 3 s | H3 |
| `resultRetryInterval` | handler | 2 s | H5 |
| `resultRetryTimeout` | handler | 30 s | H5 |
| `tombstoneTtl` | both | 60 s | C5 / H5 |
| `maxConcurrentExecutions` | handler | 1 | H4 — serial by default |
| `maxActiveCommands` | handler | 32 | H2/H8 — admission cap; slot held from admission through RESULT-CACHED, freed at the tombstone transition |

The cross-endpoint ratios (`staleTimeout` vs the peer's `heartbeatInterval`) are the deployer's
responsibility; the recommended ratios above keep single-message loss from producing false
timeouts.

## 9. Reliability properties and residual caveats

**Guarantee:** a command executes **at most once per handler process lifetime, for as long as the
handler retains dedup/tombstone state for its id** — and the commander observes exactly one
terminal outcome per command. Delivery of that outcome is at-least-once (result retries) with
duplicate suppression on both sides (C5, H1).

Residual cases — inherent, documented rather than papered over:

1. **Execution after the caller went terminal.** A `req` still in flight — broker-queued (e.g.
   QoS 1, handler offline) or a retry racing the terminal — can reach the handler after the
   commander already finished with `$noAck`, `$busy`, or `$cancelled`. Nothing distinguishes it
   from a fresh command: the handler admits and executes it once; result retries go unanswered
   (or are tombstone-`rak`ed, C5) and stop at `resultRetryTimeout`. From the caller's view the
   call failed; the side effect may still have happened. Handlers whose side effects matter
   SHOULD be idempotent per command id.
2. **Busy/loss ambiguity.** If every `$busy` reply is lost, the commander ends with `$noAck`.
   Under total message loss "full" and "unreachable" are indistinguishable by any protocol.
3. **Handler restart.** Dedup state is in-memory. If the handler restarts while a commander is
   still in SENDING (retrying `req`), the fresh instance re-admits and re-executes the command —
   the at-most-once guarantee is per process lifetime. If the restart happens after the commander
   went LIVE, retries have stopped: heartbeats cease and the commander ends with
   `$executionStale`; the partially-executed work is the idempotency caveat of case 1.
4. **Ordering.** MQTT orders messages per topic per connection; the protocol nevertheless
   tolerates loss, duplication, and reordering of every message kind (C8, H1, H6, C5).
5. **Late duplicates after tombstone eviction.** Dedup state is bounded: after `tombstoneTtl` (or
   an H8 tombstone-pool eviction) the id is forgotten. A duplicate `req` delivered later — e.g.
   QoS 1 redelivery on a persistent session after a connection drop — is admitted as new and
   executes a second time within the same process. Deployments using QoS 1 SHOULD size
   `tombstoneTtl` and the tombstone pool above their worst-case redelivery horizon (QoS 0 has no
   broker redelivery); the idempotency advice of case 1 applies.

## 10. Conformance vectors

Language-neutral test vectors are the anti-drift net: every implementation replays the same
scenarios against its protocol core over a scripted virtual clock and transport. Vectors live in
`docs/conformance/` and land together with the first implementation; this section fixes their
format and required coverage.

Each vector is one JSON file:

```jsonc
{
  "name": "queued-command-heartbeats",
  "role": "handler",                     // side under test: "commander" | "handler"
  "rules": ["H1", "H3"],                 // the C/H clauses this vector pins
  "config": { "heartbeatInterval": 3000 /* §8 keys in ms/counts; omitted keys use defaults */ },
  "steps": [
    { "at": 0,    "in":  { "topic": "request", "msg": { "v":1, "t":"req", "id":"a", "p":{} } } },
    { "at": 0,    "expect": [ { "topic": "heartbeat", "msg": { "v":1, "t":"hb", "id":"a" } } ] },
    { "at": 3000, "expect": [ { "topic": "heartbeat", "msg": { "v":1, "t":"hb", "id":"a" } } ] }
  ],
  "outcomes": { }                        // commander-role only: expected terminal outcome per call id
}
```

Every step is `{ "at": <virtual-clock ms>, <one directive> }`. Directives:

| Directive | Roles | Meaning |
|---|---|---|
| `"in": { "topic": T, "msg": {...} }` | both | deliver an inbound publish |
| `"in": { "topic": T, "raw": "..." }` | both | deliver a raw UTF-8 payload verbatim (robustness negatives) |
| `"call": { "id": I, "p": ..., "options": {...} }` | commander | start a call; `I` is the id the (scripted) generator returns; `options` = per-call §8 overrides |
| `"cancel": I` | commander | trigger local cancellation of call `I` |
| `"handler": { "id": I, "action": A, ... }` | handler | scripted consumer-stub action for admitted command `I`: `A` = `"complete"` (with `"p"`), `"fail"` (with `"e"`, optional `"m"`/`"p"`), or `"progress"` (with `"p"`). Time-scripted and non-reactive — a stub's response to cancellation is expressed by what the script does after the `cnl` step |
| `"connected": true` | both | fire the transport adapter's connected signal |
| `"expect": [ ... ]` | both | the **exact set** of outbound actions since the previous step, order-insignificant: `{ "topic": T, "msg": {...} }` for publishes, `{ "subscribe": T }` for subscriptions |

For `role:"commander"` vectors, `outcomes` maps every started call id to its expected terminal —
`{ "ok": true, "p": ... }` or `{ "ok": false, "e": "..." }` — so multi-in-flight vectors are
expressible.

Determinism rules: all timer expirations due at or before a step's `at` fire before that step
executes; steps sharing an `at` run in file order; command ids in commander vectors come from the
scripted `call.id` values (the injectable §4.2 generator is bypassed by the runner).

Required scenarios (minimum set):

1. happy path — req → hb → res → rak
2. request retry until first heartbeat (C1/C2)
3. queued-command heartbeats on a serial handler (H3)
4. duplicate req while executing → immediate hb, no re-execution (H1)
5. duplicate req while result-cached → res re-publish (H1)
6. duplicate req after tombstone → ignored (H1)
7. result retry until rak (H5)
8. late res after commander terminal → tombstone rak re-emit (C5)
9. `$busy` stateless re-emit while full (H2)
10. lost `$busy` self-heal via request retry (H2)
11. dedup precedes capacity — duplicate of admitted id on a full handler → hb, not `$busy` (H1)
12. cancel mid-execution → `$cancelled` (C7/H7)
13. cancel racing completion → cached res re-published, commander accepts it (H7)
14. cancel for unknown id → stateless `$cancelled` (H7)
15. `$noAck` expiry (C3)
16. `$executionStale` expiry (C3)
17. `$executionTimeout` expiry with live heartbeats (C3)
18. foreign-id traffic ignored — two commanders, one topic set (C6)
19. rak for unknown id ignored (H6)
20. progress delivery + progress resets the sliding stale timer (C2/C8)
21. cancel while SENDING — `req` retries stop, `cnl` retried, stateless `$cancelled` terminal
    (C7/H7)
22. cancel while QUEUED — dequeued, `$cancelled` result cached + retried (H7)
23. `resultRetryTimeout` give-up — no `rak` ever; retries stop, id tombstones (H5)
24. `res` as the first sign of life — SENDING → terminal directly, no heartbeat seen (C2/C4)
25. commander does not `rak` a `$busy` result (C4)
26. `$busy` received in LIVE is ignored, command completes normally (C4)
27. handler completes successfully despite `cnl` — genuine result wins (H7)
28. robustness: unknown `v`, unknown `t`, and type-on-wrong-topic all ignored (§4)
29. robustness: unparseable payload, missing required field (`id`; `ok` on `res`; `e` when
    `ok:false`), and invalid id all ignored (§4/§4.2)
30. robustness: unknown extra fields tolerated; omitted `p` accepted (§4)
31. multi-in-flight commander — two overlapping calls, interleaved `hb`/`res`, distinct outcomes
    (C1–C5, C6)
32. `maxConcurrentExecutions: 2` — two ids execute concurrently, both heartbeat, results
    interleave (H3/H4)
33. mixed pool — `maxActiveCommands: 1`: id A admitted and executing, id B gets stateless `$busy`,
    duplicate of A still gets `hb` not `$busy` (H1/H2)
34. reconnect — connected signal fires; all subscriptions re-established, in-flight timers
    unaffected (§2)
35. commander tombstone expiry — a `res` redelivered after `tombstoneTtl` elicits nothing (C5)
