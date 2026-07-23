# ParleyMqtt

[![CI](https://img.shields.io/github/actions/workflow/status/serionist/parley-mqtt/ci.yml?branch=main&event=push&label=CI)](https://github.com/serionist/parley-mqtt/actions/workflows/ci.yml)
[![PlatformIO Registry](https://badges.registry.platformio.org/packages/serionist/library/ParleyMqtt.svg)](https://registry.platformio.org/libraries/serionist/ParleyMqtt)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/serionist/parley-mqtt/blob/main/LICENSE)

**Request/response RPC over MQTT for ESP32/Arduino** — the C++ reference implementation of the
Parley v1 protocol.

```ini
; platformio.ini
lib_deps = serionist/ParleyMqtt
```

or `pio pkg install -l "ParleyMqtt"`. ArduinoJson is pulled in as a dependency.

Parley turns the MQTT broker your devices already share into something you can make *requests*
over — request/reply, not just publish/subscribe. Call a command on another device, stream
progress while it runs, cancel it, and receive one typed result: success or a clear error. Each
command is four topic strings you choose, and every message carries a correlation id, so many
calls share the same topics.

One wire protocol, four parallel reference libraries — **C++ (ESP32/Arduino)**, **C#**,
**TypeScript/JS** and **Python** — so firmware, a .NET service, an edge script and a Python
service can all call each other over the broker they already share. `ParleyMqtt` is the C++ one.

- **Statically allocated.** No heap allocation in the protocol core, no exceptions, no RTTI, no
  `std::function` — fixed-capacity tables sized by template parameters, callbacks as plain
  function pointers plus a `void*`. Driven from your `loop()` with `tick(millis())`.
- **No MQTT-client dependency.** Parley never opens a connection and never sees your broker
  credentials or TLS material; you hand it a live session through a small transport adapter you
  write over AsyncMqttClient, PubSubClient (note: its maintainer marked it unmaintained
  in 2026), or whatever you already use. It works over MQTT
  3.1.1, so it does not need an MQTT 5 stack.
- **Three distinct liveness outcomes.** *Nobody answered* (`$noAck`), *it accepted and then went
  silent* (`$executionStale`) and *it ran too long* (`$executionTimeout`) are separate results,
  driven by per-execution heartbeats rather than one guessed stopwatch.
- **At-most-once execution, correct even at QoS 0.** Requests are re-sent until the other side
  shows a sign of life, results are re-sent until acknowledged, and duplicates are recognized and
  dropped on both sides. The guarantee is *at most once per handler process lifetime, for as long
  as the handler retains dedup/tombstone state for that command id* — the residual cases are
  written down in
  [§9 of the protocol spec](https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md#9-reliability-properties-and-residual-caveats).
- **Cancellation that reaches the running handler** — a real cancel on the wire, not a local
  give-up.

```cpp
#include <Parley.h>
using namespace parley;

// Callbacks are plain function pointers + a void* userData (no std::function).
static void onComplete(void* ud, const ParleyResult<BackupResult, BackupError>& r) {
  if (r.isSuccess()) Serial.printf("wrote %ld bytes\n", r.value().bytesWritten);
  else Serial.printf("failed: %s\n", r.error().code());
}

ParleyTopics topics("home/backup/req", "home/backup/hb", "home/backup/prg", "home/backup/res");
ParleyCommander<BackupRequest, BackupProgress, BackupResult, BackupError, /*MaxInFlight=*/2>
    commander(adapter, topics);

BackupRequest req; strcpy(req.target, "photos");
ParleyCallHandle handle = commander.execute(req, onComplete, &backupCall);

void loop() {
  mqtt.loop();
  commander.tick(millis());   // ... commander.cancel(handle) to cancel
}
```

## Example sketch

`examples/Backup/Backup.ino` ships with the package: the same `backup` command as a handler and
as a commander, with an illustrative PubSubClient adapter. Open it from the PlatformIO/Arduino
examples list, or read it
[on GitHub](https://github.com/serionist/parley-mqtt/blob/main/cpp/examples/Backup/Backup.ino).

## Documentation

- **Repository:** <https://github.com/serionist/parley-mqtt>
- **The wire protocol (normative):** <https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md>
- **Design notes:** <https://github.com/serionist/parley-mqtt/blob/main/docs/design.md>
- **Testing & conformance:** <https://github.com/serionist/parley-mqtt/blob/main/docs/testing.md>

## License

[MIT](https://github.com/serionist/parley-mqtt/blob/main/LICENSE) © The Parley Authors.
