# Parley.Mqtt

**Request/response RPC over MQTT** — the transport-free protocol core, C# reference implementation.

Parley turns the MQTT broker you already share into something you can make *requests* over:
call a command on another device, stream progress while it runs, cancel it, and receive one
typed result — success or a clear error. It is a small library, not a server: it rides your
existing MQTT connection and never holds your broker credentials or TLS material.

One wire protocol, four parallel reference libraries — **C#**, **TypeScript/JS**, **Python**, and
**C++ (ESP32/Arduino)** — so a .NET service, an edge script, a Python service, and firmware can all
call each other. `Parley.Mqtt` is the C# one.

- Targets `net10.0` and `netstandard2.0`; trim- and AOT-friendly via source-generated JSON.
- Zero MQTT-client dependency — you supply a live connection through a tiny adapter you write.
- At-most-once execution correct even at QoS 0; retries, dedup, heartbeats, progress, real
  cancellation, and six built-in error codes.

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

- **Repository:** <https://github.com/serionist/parley>
- **The wire protocol (normative):** <https://github.com/serionist/parley/blob/main/docs/protocol.md>
- **Worked C# example:** <https://github.com/serionist/parley/tree/main/examples/csharp>
- **Design notes:** <https://github.com/serionist/parley/blob/main/docs/design.md>

## License

[MIT](https://github.com/serionist/parley/blob/main/LICENSE) © The Parley Authors.
