# Parley end-to-end interop matrix

This is **layer 3** of [`docs/testing.md`](../docs/testing.md): the language-neutral interop suite run
over a **real** dockerized MQTT broker. It proves that the four reference libraries — C#, JS/TS,
Python, C++ — actually interoperate on the wire, not merely that each passes the shared conformance
vectors (layer 2). If two implementations both pass the vectors, a matrix failure can only come from
real-world residue: codec quirks, client behaviour, QoS, timing.

## What runs

- One pinned **mosquitto 2.0.22** broker (`mosquitto.conf`: `listener 1883`, `allow_anonymous true`,
  `persistence false` — secretless and stateless; no cross-run redelivery).
- One **test peer** image per language. Each peer is a *generic interpreter* of
  [`../docs/conformance/interop/scenarios.json`](../docs/conformance/interop/scenarios.json) and holds
  **zero suite logic**: the handler role serves the five fixed harness command behaviours
  (`echo`, `slow`, `err`, `cancelme`, `busy1`), the commander role runs the step model and writes a
  JSON report. Peers publish/subscribe at **QoS 0** (the protocol baseline).
- `run-matrix.sh` runs all **16 ordered commander × handler pairs** serially on isolated topic
  prefixes, plus the **4 per-commander `$noAck` self-checks** (which test no pair — a commander alone
  against a handler-less prefix). The diagonal (X→X) doubles as each language's real-broker
  integration test.

## Peer CLI

```
parley-testpeer --role handler   --broker <host:port> --prefix <topic-prefix> [--scenarios <path>]
parley-testpeer --role commander --broker <host:port> --prefix <topic-prefix> \
                [--suite matrix|selfchecks] [--report <path>] [--handler-lang <name>]
```

The handler subscribes under `<prefix>/<command>/{req,hb,prg,res}` and prints `PARLEY-PEER-READY`
once every subscription is acknowledged by the broker. The commander runs the suite, writes
`{ commander, handler, prefix, suite, results:[{scenario, pass, detail}], pass }`, and exits nonzero
on any failure.

## Run it

```sh
cd e2e
./run-matrix.sh r1        # build + broker + 16 pairs + 4 self-checks; prints the 4×4 grid
./run-matrix.sh r2        # a second, prefix-isolated run — the determinism gate
docker compose down -v    # tear down the broker when done
```

### Result classification

| Cell | Meaning |
|---|---|
| `GREEN` | passed on the first attempt |
| `AMBER` | passed only on the single retry — a flake. **Blocks the gate**; the report carries the first-attempt diff. |
| `RED` | failed both attempts — carries both diffs and flags a cross-attempt divergence (different failing scenarios = a nondeterminism signal). |

`run-matrix.sh` exits `0` only when **every** cell is GREEN; `1` on any AMBER; `2` on any RED. Each
pair is gated on the handler's READY line and on a per-pair wall-clock timeout (a hung peer becomes
RED rather than hanging the matrix).

## Pinned versions

| Component | Version |
|---|---|
| mosquitto (broker image) | `eclipse-mosquitto:2.0.22` |
| C# — MQTTnet | `5.2.0.1603` (.NET base `mcr.microsoft.com/dotnet/{sdk,runtime}:10.0`) |
| JS — mqtt.js / tsx | `5.15.2` / `4.23.1` (base `node:24-bookworm-slim`) |
| Python — paho-mqtt | `2.1.0` (base `python:3.13-slim-bookworm`) |
| C++ — libmosquitto / ArduinoJson | Debian bookworm `2.0.11` / vendored `7.4.3` (base `debian:bookworm-slim`) |

The C++ peer replaces the Arduino-only PubSubClient with **libmosquitto**, pumped **synchronously**
(`mosquitto_loop()` in the peer's main loop beside `tick()`, no threads) — exactly the Arduino
`loop()` model the tick-driven core is built for.

## Recorded gap

QoS-1 duplicate absorption stays unexercised over the wire in v1 — every adapter publishes/subscribes
at QoS 0 (the protocol baseline). Dedup/tombstone behaviour under QoS-1 redelivery is covered by the
layer-2 vectors, not here.
