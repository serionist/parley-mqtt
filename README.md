# Parley

**Parley is a small cross-platform library for request/response RPC over MQTT.** One wire
protocol so the different parts of a home/automation system — a .NET service, a Python service,
an ESP32 firmware, a JavaScript edge script — can call each other and get answers back, over the
MQTT broker and connection they already have.

*A "parley" is a conversation between two parties to reach agreement — the shape of an RPC call.*

**Status:** the wire protocol ([`docs/protocol.md`](docs/protocol.md), normative) and the
cross-language design ([`docs/design.md`](docs/design.md)) are written; the four reference
libraries are not built yet. Rationale for the design lives in
[`docs/decisions.md`](docs/decisions.md).

## Why

MQTT is everywhere in automation, but it's pub/sub — there's no standard "call this and wait for
the result". People bolt RPC on ad-hoc and it breaks on exactly the devices that need it most.
Parley makes it a proper, reusable thing that stays thin enough for constrained targets.

## Main functionalities

- **Request/response RPC over MQTT** — execute a command on another endpoint and await its
  typed result, with progress along the way.
- **Bring your own connection** — the library is handed a live MQTT session through a small
  adapter you implement over your existing client. It never connects, never holds credentials or
  TLS material, and carries no MQTT client dependency of its own.
- **MQTT-version agnostic** — works on any broker from MQTT 3.1.1 up; correlation, typing, and
  versioning live in small JSON payloads, not MQTT 5 properties. Correct at QoS 0.
- **Your topics, not ours** — each command type gets four topic strings (request, heartbeat,
  progress, result) that *you* choose; no namespace or naming scheme is imposed, so broker ACLs
  stay entirely in your hands.
- **Reliable without fuss** — requests are re-sent until the handler shows life; results are
  re-sent until acknowledged; duplicates are de-duplicated by command id. At-most-once execution
  without QoS 2.
- **Liveness per command** — the handler heartbeats every accepted command (even while it's
  queued), so "the peer is gone" is never confused with "the job is slow" — and a silent-but-alive
  long job needs no fake progress to stay trusted.
- **Progress + cancellation for long operations** — advisory progress updates during a slow call,
  and a wire-level cancel that actually reaches the running handler.
- **Errors that tell you what happened** — builtin codes distinguish never-accepted (`$noAck`),
  handler-full (`$busy`), went-silent (`$executionStale`), never-finished (`$executionTimeout`),
  and cancelled (`$cancelled`) — plus your own typed error codes for domain failures.
- **Typed with your language's own tools** — generics/templates/type hints bind request,
  progress, result, and error types; no schema compiler, no code generation.
- **Four languages, one protocol** — C# (.NET), Python, JS/TS (Node + edge engines), and C++
  (ESP32/Arduino), all interoperable and all shaped alike: a `ParleyCommander` to call, a
  `ParleyCommandHandler` to serve.

## How a call works

```
commander ──req──▶  handler        re-sent until the handler shows life
          ◀──hb───                 heartbeat from admission — the first one is the accept-ack
          ◀──prg─── (×N)           progress — advisory, never acked
          ◀──res──                 the single terminal result — re-sent until acked
          ──rak──▶                 result-ack stops the re-sends
          (──cnl──▶)               cancel, any time before the result
```

Everything is a small JSON message carrying a command id on one of four consumer-chosen topics.
The full contract — state machines, timeouts, error codes, conformance vectors — is
[`docs/protocol.md`](docs/protocol.md).

## Not in scope

Not a transport (it rides your existing MQTT connection), not a security layer (that's broker
ACL + TLS on your connection), not a presence system (no retained heartbeats or Last-Will — you
own the connection, so presence is yours if you want it), not a pub/sub replacement (telemetry
stays plain MQTT), and no code generation (bind types with your language's own tools).
