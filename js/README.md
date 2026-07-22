# parley-mqtt

**Request/response RPC over MQTT** — the TypeScript/JS reference implementation of the Parley v1 protocol.

Parley turns the MQTT broker you already share into something you can make *requests* over:
call a command on another device, stream progress while it runs, cancel it, and receive one
typed result — success or a clear error. It is a small library, not a server: it rides your
existing MQTT connection and never holds your broker credentials or TLS material.

One wire protocol, four parallel reference libraries — **C#**, **TypeScript/JS**, **Python**, and
**C++ (ESP32/Arduino)** — so a .NET service, an edge script, a Python service, and firmware can all
call each other. `parley-mqtt` is the JS/TS one.

- Ships ESM + CJS + `.d.ts`; TypeScript-strict, discriminated-union results.
- Zero runtime dependencies — you supply a live MQTT connection through a tiny adapter you write.
  Runs on Node 18+, Deno, Bun, and edge engines.
- At-most-once execution correct even at QoS 0; retries, dedup, heartbeats, progress, real
  cancellation, and six built-in error codes.

```bash
npm install parley-mqtt
```

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

- **Repository:** <https://github.com/serionist/parley>
- **The wire protocol (normative):** <https://github.com/serionist/parley/blob/main/docs/protocol.md>
- **Worked TypeScript example:** <https://github.com/serionist/parley/tree/main/examples/js>
- **Design notes:** <https://github.com/serionist/parley/blob/main/docs/design.md>

## License

[MIT](https://github.com/serionist/parley/blob/main/LICENSE) © The Parley Authors.
