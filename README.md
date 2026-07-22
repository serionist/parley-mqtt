# Parley — brief

**Parley is a small cross-platform library for request/response RPC over MQTT.** One protocol so the
different parts of a home/automation system — a .NET service, an ESP32 firmware, a JavaScript edge
script (e.g. Shelly) — can call each other and get answers back, over the MQTT broker they already
share.

*A "parley" is a conversation between two parties to reach agreement — the shape of an RPC call.*

**Status:** just this brief. Nothing is built yet; this is the idea we want to build out later as a
standalone open-source library (C# · C++ · JS/TS), consumed by other projects (e.g. as a package or a
git submodule).

## Why

MQTT is everywhere in automation, but it's pub/sub — there's no standard "call this and wait for the
result". People bolt RPC on ad-hoc and it breaks on the exact devices that need it most. Parley makes
it a proper, reusable thing that works even on constrained devices.

## Main functionalities

- **Request/response RPC over MQTT** — call a method on another endpoint and await its result.
- **Works on plain MQTT 3.1.1 and tiny buffers** — correlation lives in the message payload, not in
  MQTT 5 request/response properties (which embedded MQTT clients can't use).
- **ACL-friendly topics** — each endpoint has one fixed inbox; concurrent calls are told apart by an id
  in the payload, so static broker ACLs work.
- **Reliable without fuss** — safe over flaky links: a call runs at most once, and the answer is
  redelivered until it's acknowledged (retransmit + de-duplication, no need for heavy QoS 2).
- **Liveness built in** — a first-class, library-managed heartbeat on its own topic (with a retained
  Last-Will) tells you whether a peer is alive, without making a call.
- **Progress for long operations** — stream progress updates during a slow call (e.g. a backup/update),
  separate from the heartbeat.
- **Sensible timeouts** — distinguishes "never accepted", "accepted but timed out", and "went silent
  mid-progress", instead of one blunt timeout.
- **Typed, schema-driven** — declare your methods in a schema and generate typed client/server code per
  language. The library itself stays standalone; you generate the types when you use it.
- **Three target languages, interoperable** — C# (.NET service), C++ (ESP32/Arduino firmware), and
  JS/TS (Node + edge engines), all speaking the same wire protocol.

## How it works (the RPC mechanics)

Everything is small JSON messages carrying a **correlation id**, published over a handful of fixed
topics. Correlation lives in the payload (not in MQTT 5 properties) so it works on plain MQTT 3.1.1 and
tiny embedded buffers.

### Topics
For a deployment namespace `<ns>` and stable endpoint ids:

| Topic | Direction |
|---|---|
| `<ns>/rpc/req/<callee>` | callers publish requests here; the callee subscribes |
| `<ns>/rpc/res/<caller>` | the callee publishes replies here; the caller subscribes (its **one** inbox) |
| `<ns>/rpc/hb/<endpoint>` | each endpoint publishes its heartbeat here (retained) |
| `<ns>/rpc/last/<callee>/<method>` | *(optional)* retained "last result" slot, overwritten each call |

Each caller has **one fixed inbox** — concurrent calls are told apart by the id in the message, so static
broker ACLs work. Requests and replies are never retained; heartbeats (and the optional last-result slot)
are.

### The four message kinds (a call's life)
```
caller  ──req──▶  callee          req   the call: method + params + expiry
        ◀──ack──                  ack   "accepted, executing" (sent once)
        ◀──prog── (×N)            prog  progress updates — advisory, never acked
        ◀──res──                  res   the single terminal result: success OR error
```

### Timeouts — three horizons, not one
- **ack timeout** — did the callee accept? (peer down / unreachable)
- **execution timeout** — it accepted, but did it ever finish?
- **progress-stall** *(optional)* — it was progressing, then went silent.

Distinguishing these tells "the peer is gone" apart from "the job is just slow".

### Automatic resends (reliable over flaky links)
- The **caller** re-sends the same request (same id) until it gets an ack/result, or retries run out →
  then it fails with a no-ack error. You don't write retry loops.
- The **callee** de-duplicates by id: it runs a given call **at most once**, and just re-acks or
  re-sends the cached result if a duplicate arrives. So at-least-once delivery is made safe **without**
  heavy QoS 2.

### Heartbeat (liveness, separate from progress)
Each endpoint publishes a small retained heartbeat on its own `hb/<endpoint>` topic on a timer, and
registers a Last-Will that flips it to "down" if it drops ungracefully. A watcher just subscribes to know
whether a peer is alive — no call needed. This is deliberately distinct from per-call progress: a long,
silent-but-alive job still heartbeats while emitting no progress.

## Not in scope

Not a transport (it rides your existing MQTT broker), not a security layer (that's broker ACL + TLS),
not a pub/sub replacement (telemetry stays plain MQTT).
