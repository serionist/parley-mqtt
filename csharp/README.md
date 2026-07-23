# Parley.Mqtt

[![CI](https://img.shields.io/github/actions/workflow/status/serionist/parley-mqtt/ci.yml?branch=main&event=push&label=CI)](https://github.com/serionist/parley-mqtt/actions/workflows/ci.yml)
[![NuGet](https://img.shields.io/nuget/v/Parley.Mqtt?label=NuGet)](https://www.nuget.org/packages/Parley.Mqtt)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/serionist/parley-mqtt/blob/main/LICENSE)

**Request/response RPC over MQTT** — the transport-free protocol core, C# reference implementation.

```console
dotnet add package Parley.Mqtt
```

Parley turns the MQTT broker you already share into something you can make *requests* over —
request/reply, not just publish/subscribe. Call a command on another service or device, stream
progress while it runs, cancel it, and receive one typed result: success or a clear error. Each
command is four topic strings you choose, and every message carries a correlation id, so many
calls share the same topics.

One wire protocol, four parallel reference libraries — **C#**, **TypeScript/JS**, **Python** and
**C++ (ESP32/Arduino)** — so a .NET service, an edge script, a Python service and firmware can all
call each other over the broker they already share. `Parley.Mqtt` is the C# one (the assembly and
namespace stay `Parley`). It is a small library, not a server: it rides your existing MQTT
connection and never holds your broker credentials or TLS material.

- **Targets `net10.0` and `netstandard2.0`.** Trim- and AOT-friendly via source-generated JSON;
  request, progress, result and error types are bound with ordinary generics — no schema compiler,
  no code generation, no build step.
- **Zero MQTT-client dependency.** You supply a live connection through a tiny adapter you write
  over MQTTnet or whatever you already use, and Parley needs only MQTT 3.1.1 — no MQTT 5 broker
  required.
- **Three distinct liveness outcomes.** *Nobody answered* (`$noAck`), *it accepted and then went
  silent* (`$executionStale`) and *it ran too long* (`$executionTimeout`) are separate results,
  driven by per-execution heartbeats rather than one guessed stopwatch.
- **At-most-once execution, correct even at QoS 0.** Requests are re-sent until the other side
  shows a sign of life, results are re-sent until acknowledged, and duplicates are recognized and
  dropped on both sides. The guarantee is *at most once per handler process lifetime, for as long
  as the handler retains dedup/tombstone state for that command id* — the residual cases are
  written down in
  [§9 of the protocol spec](https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md#9-reliability-properties-and-residual-caveats).
- **Cancellation that reaches the running handler** — a `CancellationToken` puts a real cancel on
  the wire, not a local give-up. Plus advisory progress streaming (`IProgress<T>`) and six built-in
  error codes alongside your own.

```csharp
var topics = new ParleyTopics("home/backup/req", "home/backup/hb", "home/backup/prg", "home/backup/res");
using var commander = new ParleyCommander<BackupRequest, BackupProgress, BackupResult, string>(
    adapter, topics, new ParleyConfig(), BackupJsonContext.Default);

var progress = new Progress<BackupProgress>(p => Console.WriteLine($"{p.Percent}%"));
ParleyResult<BackupResult, string> result =
    await commander.ExecuteAsync(new BackupRequest("photos"), progress: progress);

if (result.IsSuccess) Console.WriteLine(result.Value!.BytesWritten);
else Console.WriteLine(result.Error.Code);
```

## Documentation

- **Repository:** <https://github.com/serionist/parley-mqtt>
- **The wire protocol (normative):** <https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md>
- **Worked C# example:** <https://github.com/serionist/parley-mqtt/tree/main/examples/csharp>
- **Design notes:** <https://github.com/serionist/parley-mqtt/blob/main/docs/design.md>

## License

[MIT](https://github.com/serionist/parley-mqtt/blob/main/LICENSE) © The Parley Authors.
