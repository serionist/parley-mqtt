# Parley examples

Four self-contained examples — one per language — that all model the **same**
command so you can compare them side by side: a `backup` command that takes a
target, streams progress while it runs, returns how many bytes it wrote, and can
fail with typed error codes (`diskFull`, `targetUnreachable`).

| Language | File | MQTT client used in the adapter |
|---|---|---|
| TypeScript / JS | [`js/backup-example.ts`](js/backup-example.ts) | [mqtt.js](https://github.com/mqttjs/MQTT.js) |
| Python | [`python/backup_example.py`](python/backup_example.py) | [paho-mqtt](https://github.com/eclipse-paho/paho.mqtt.python) |
| C# / .NET | [`csharp/BackupExample.cs`](csharp/BackupExample.cs) | [MQTTnet](https://github.com/dotnet/MQTTnet) |
| C++ / ESP32 | [`cpp/backup_example.ino`](cpp/backup_example.ino) | [PubSubClient](https://github.com/knolleary/pubsubclient) |

Each file walks through the same five steps: define the message types → write a
transport adapter over an MQTT client → construct the commander/handler → serve
or call the command (with progress and cancellation) → tear down.

## What is and isn't verified

Please read this before copying anything into production.

- **The library-facing code is compile-checked.** Everything that touches Parley
  itself — the type bindings, the handler subclass, constructing the
  commander/handler, `execute`/`ExecuteAsync`, reading the result, teardown — was
  compiled/type-checked against the local libraries while these examples were
  written (with the illustrative adapter swapped for an in-memory one). It uses
  the real public API.

- **The transport adapters are ILLUSTRATIVE.** Each example includes a small
  adapter over a popular MQTT client (mqtt.js, paho-mqtt, MQTTnet, PubSubClient).
  Those adapters were hand-checked against each client's current API but are **not
  packaged and not built in CI** — they show the *shape* you implement over your
  own client, not a supported dependency. Every adapter is marked with a header
  comment saying so.

- **Verified, runnable peers are coming.** The end-to-end test matrix
  ([`docs/testing.md`](../docs/testing.md)) ships one small verified peer per
  language over a real broker. When that lands, these illustrative adapters get
  battle-tested equivalents.

## Running them

From v1.0.0 the libraries ship on the standard registries — `parley-mqtt` (npm,
PyPI), `Parley.Mqtt` (NuGet), `ParleyMqtt` (PlatformIO); install per the
top-level [`README.md`](../README.md), or consume straight from this repository. You also need a running MQTT broker — a local
[Mosquitto](https://mosquitto.org/) is perfect. Start the handler in one process
and the commander in another; they meet on the broker.

All topics in the examples are placeholders (`home/backup/req`, …) — replace them
with topics your broker's ACLs allow.
