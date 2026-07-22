# Parley end-to-end interop matrix

This is **layer 3** of [`docs/testing.md`](../docs/testing.md): the language-neutral interop suite run
over a **real** dockerized MQTT broker. It proves that the four reference libraries — C#, JS/TS,
Python, C++ — actually interoperate on the wire, not merely that each passes the shared conformance
vectors (layer 2). If two implementations both pass the vectors, a matrix failure can only come from
real-world residue: codec quirks, client behaviour, QoS, timing, reconnect.

## What runs

`run-matrix.sh` builds the five pinned images, brings up the healthcheck-gated broker, then runs three
phases:

1. **BASE grid** — all **16 ordered commander × handler pairs**, each running the **9 base scenarios**
   (`echo`, `slow`, `err`, `cancelme`, `busy1`, `concurrent`, `churn`, `boundary-echo`, `silent-slow`)
   at QoS 0, plus the **4 per-commander `$noAck` self-checks** (which test no pair — a commander alone
   against a handler-less prefix). The diagonal (X→X) doubles as each language's real-broker
   integration test.
2. **DEEP phase** — driver-orchestrated real-world disruptions on a **fixed, deterministic pair
   mapping** (`csharp→js`, `js→python`, `python→cpp`, `cpp→csharp`), identical across runs. Each pair
   runs both deep scenarios from
   [`../docs/conformance/interop/deep-scenarios.json`](../docs/conformance/interop/deep-scenarios.json):
   - `stale-detect` — the commander fires `slow(8000ms)`, and at its barrier the driver **SIGKILLs**
     the handler container. Heartbeats cease and the commander terminates `$executionStale`.
   - `broker-restart` — the commander fires `slow(8000ms)`, and at its barrier the driver **bounces
     mosquitto** (`docker compose restart -t 1`). Both peers reconnect, the library re-subscribes on
     the connected signal, the handler's execution bridges the outage, and the call completes `{ok:true}`.
3. **QoS-1 pass** — the full 16-pair grid re-run with `--qos 1` on both peers.

Each peer is a *generic interpreter* of the scenario files and holds **zero suite logic**: the handler
role serves the five fixed harness command behaviours; the commander role runs the step model and writes
a JSON report.

## Peer CLI

```
parley-testpeer --role handler   --broker <host:port> --prefix <topic-prefix> \
                [--scenarios <path>] [--qos 0|1]
parley-testpeer --role commander --broker <host:port> --prefix <topic-prefix> \
                [--scenarios <path>] [--suite matrix|selfchecks] [--scenario <name>] \
                [--qos 0|1] [--report <path>] [--handler-lang <name>]
```

The handler subscribes under `<prefix>/<command>/{req,hb,prg,res}` and prints `PARLEY-PEER-READY` once
every subscription is acknowledged by the broker. The commander runs the suite, writes
`{ commander, handler, prefix, suite, results:[{scenario, pass, detail}], pass }`, and exits nonzero on
any failure.

## Cross-language peer contract

All four peers implement these additions **identically** (the divergence-is-a-smell rule):

- **`--qos {0|1}`** — the adapter publishes/subscribes at the given QoS (retain always `false`). Default 0.
- **`--scenario <name>`** — single-scenario mode: run only the named scenario from the selected suite.
- **`barrier` step** — a deep-scenario directive that prints a **flushed** `PARLEY-BARRIER <label>` line
  to stdout and continues. It carries no protocol semantics; it is purely the rendezvous the driver
  watches for over the container log stream (chosen over a broker-topic barrier precisely because the
  mosquitto restart would tear a topic barrier down).
- **reconnect** — each adapter fires the library's *connected* callback on every (re)connection, and the
  core re-subscribes in response (protocol.md §2). How each client gets there:
  - **C# (MQTTnet)** — the raw `IMqttClient` does not auto-reconnect, so the adapter runs a manual
    reconnect loop (`ConnectAsync` until it succeeds) on an unexpected disconnect; each successful
    connect fires `ConnectedAsync` → `RaiseConnected()`.
  - **C++ (libmosquitto)** — the synchronous pump calls `mosquitto_reconnect` on a loop error and, when
    a new connection generation appears, fires `connected()` on every core.
  - **JS (mqtt.js)** — built-in auto-reconnect refires the `connect` event; the client is created with
    `resubscribe: false` so the **library** is the sole resubscriber.
  - **Python (paho)** — the `loop_start` thread auto-reconnects (backoff capped at 2 s) and refires
    `on_connect` → `raise_connected`.

## Run it

```sh
cd e2e
./run-matrix.sh r1        # build + broker + base grid + self-checks + deep phase + qos-1 pass
./run-matrix.sh r2        # a second, prefix-isolated run — the determinism gate (same fixed deep pairs)
docker compose down -v    # tear down the broker when done
```

Reports land in `reports/` (git-ignored; regenerated every run). Expected wall-clock is materially above
the base-only matrix (8 deep runs each hold a live command for several seconds, ×2 the QoS-1 grid);
the total is printed at the end of each run.

### Result classification

| Cell | Meaning |
|---|---|
| `GREEN` | passed on the first attempt |
| `AMBER` | passed only on the single retry — a flake. **Blocks the gate**; the report carries the first-attempt diff. |
| `RED` | failed both attempts — carries both diffs and flags a cross-attempt divergence (different failing scenarios = a nondeterminism signal). |

`run-matrix.sh` exits `0` only when **every** cell across all three phases is GREEN; `1` on any AMBER;
`2` on any RED. Base/QoS-1 pairs are gated on the handler's READY line and a per-pair wall-clock timeout;
deep runs are gated on the READY line, the barrier line, and a per-run wall-clock cap (a hung peer
becomes RED rather than hanging the matrix).

## Pinned versions

| Component | Version |
|---|---|
| mosquitto (broker image) | `eclipse-mosquitto:2.0.22` |
| C# — MQTTnet | `5.2.0.1603` (.NET base `mcr.microsoft.com/dotnet/{sdk,runtime}:10.0`, digest-pinned) |
| JS — mqtt.js / tsx | `5.15.2` / `4.23.1` (base `node:24-bookworm-slim`, digest-pinned) |
| Python — paho-mqtt | `2.1.0` (base `python:3.13-slim-bookworm`, digest-pinned) |
| C++ — libmosquitto / ArduinoJson | apt `2.0.11-1.2+deb12u2` / vendored `7.4.3` (base `debian:bookworm-slim`, digest-pinned) |

Every peer base image is pinned by digest (tag kept for readability) and the C++ `libmosquitto`
apt packages are pinned to an exact version, so a rebuild is reproducible. The C++ peer replaces the
Arduino-only PubSubClient with **libmosquitto**, pumped **synchronously** (`mosquitto_loop()` in the
peer's main loop beside `tick()`, no threads) — exactly the Arduino `loop()` model the tick-driven core
is built for.

## Recorded gap

The QoS-1 pass is **adapter-compatibility coverage only**. With clean sessions and broker persistence
off, no redelivery path exists, so it proves QoS-1 *mechanics* (PUBACK flow, no stall) — **not**
redelivery/duplicate absorption. QoS-1 dedup **over the wire** therefore remains **unclosed** here:
genuine closure would need persistent sessions + broker persistence + an induced disconnect that forces
a redelivery. Dedup/tombstone behaviour under redelivery stays covered by the layer-2 vectors. The deep
scenarios stay QoS 0 to isolate reconnect from redelivery.
