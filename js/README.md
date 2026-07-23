# parley-mqtt

[![CI](https://img.shields.io/github/actions/workflow/status/serionist/parley-mqtt/ci.yml?branch=main&event=push&label=CI)](https://github.com/serionist/parley-mqtt/actions/workflows/ci.yml)
[![npm](https://img.shields.io/npm/v/parley-mqtt?label=npm)](https://www.npmjs.com/package/parley-mqtt)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/serionist/parley-mqtt/blob/main/LICENSE)

**Request/response RPC over MQTT** — the TypeScript/JS reference implementation of the Parley v1
protocol.

```bash
npm install parley-mqtt
```

Parley turns the MQTT broker you already share into something you can make *requests* over —
request/reply, not just publish/subscribe. Call a command on another device or service, stream
progress while it runs, cancel it, and receive one typed result: success or a clear error. Each
command is four topic strings you choose, and every message carries a correlation id, so many
calls share the same topics.

One wire protocol, four parallel reference libraries — **TypeScript/JS**, **C#**, **Python** and
**C++ (ESP32/Arduino)** — so an edge script, a .NET service, a Python service and firmware can all
call each other over the broker they already share. `parley-mqtt` is the JS/TS one. It is a small
library, not a server: it rides your existing MQTT connection and never holds your broker
credentials or TLS material.

- **Ships ESM + CJS + `.d.ts`.** TypeScript-strict, discriminated-union results; request, progress,
  result and error types are bound with your own generics — no schema compiler, no code generation,
  no build step.
- **Zero runtime dependencies, no MQTT-client dependency.** You supply a live connection through a
  tiny adapter you write over mqtt.js or whatever you already use. Runs on Node 18+, Deno, Bun and
  edge engines, and needs only MQTT 3.1.1 — no MQTT 5 broker required.
- **Three distinct liveness outcomes.** *Nobody answered* (`$noAck`), *it accepted and then went
  silent* (`$executionStale`) and *it ran too long* (`$executionTimeout`) are separate results,
  driven by per-execution heartbeats rather than one guessed stopwatch.
- **At-most-once execution, correct even at QoS 0.** Requests are re-sent until the other side
  shows a sign of life, results are re-sent until acknowledged, and duplicates are recognized and
  dropped on both sides. The guarantee is *at most once per handler process lifetime, for as long
  as the handler retains dedup/tombstone state for that command id* — the residual cases are
  written down in
  [§9 of the protocol spec](https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md#9-reliability-properties-and-residual-caveats).
- **Cancellation that reaches the running handler** — aborting puts a real cancel on the wire, not
  a local give-up. Plus advisory progress streaming and six built-in error codes alongside your own.

```ts
import { ParleyCommander, ParleyTopics } from 'parley-mqtt';

const topics = new ParleyTopics('home/backup/req', 'home/backup/hb', 'home/backup/prg', 'home/backup/res');
const commander = new ParleyCommander<Req, Prog, Res, Err>(adapter, topics);

const result = await commander.execute({ target: 'photos' }, {
  onProgress: (p) => console.log(`${p.percent}%`),
  signal: AbortSignal.timeout(300_000), // aborting sends a real cancel
});
if (result.ok) console.log(result.value.bytesWritten);
```

## Documentation

- **Repository:** <https://github.com/serionist/parley-mqtt>
- **The wire protocol (normative):** <https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md>
- **Worked TypeScript example:** <https://github.com/serionist/parley-mqtt/tree/main/examples/js>
- **Design notes:** <https://github.com/serionist/parley-mqtt/blob/main/docs/design.md>

## License

[MIT](https://github.com/serionist/parley-mqtt/blob/main/LICENSE) © The Parley Authors.
