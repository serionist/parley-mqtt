# Parley

**Parley lets the different parts of your system call each other over MQTT and
get an answer back.** You already have devices and services talking to one MQTT
broker — an ESP32, a .NET service, a Python script, a bit of edge JavaScript.
Parley turns that shared broker into something you can make *requests* over: call
a command on another device, stream progress while it runs, cancel it if you
change your mind, and receive one typed result — success or a clear error.

It is a small library, not a server. It rides the MQTT connection you already
have, in four languages that all speak one wire protocol: **C#**, **TypeScript/JS**,
**Python**, and **C++ (ESP32/Arduino)**.

*A "parley" is a conversation between two sides to reach agreement — the shape of
a request and its reply.*

## Why

MQTT is everywhere in home and industrial automation, and it's wonderful at what
it does: publish/subscribe. A sensor publishes a reading; whoever cares is
subscribed. But publish/subscribe has no built-in notion of **"call this and wait
for the result."** There's no "did it work?", no return value, no "it's still
working, hang on."

So people bolt request/response on by hand — a request topic here, a reply topic
there, a correlation id stuffed in the payload, a timeout guessed at — and it
tends to break on exactly the flaky, low-power devices that need it most. Was the
job slow, or did the device die? Did my request even arrive? Did the answer come
back but I missed it?

Parley makes request/response a proper, reusable thing, once, and keeps it small
enough to run on a microcontroller.

## What you get

- **Reliable answers without heavyweight MQTT.** Requests are re-sent until the
  other side shows a sign of life; results are re-sent until you acknowledge
  them; duplicates are recognized and dropped. You get at-most-once execution
  **without needing MQTT's QoS 2** — it's correct even at QoS 0.
- **"Slow" is never mistaken for "dead."** While a handler works on your command,
  it sends a small heartbeat every few seconds. So a long-running job stays
  trusted without faking progress, and a truly unreachable device is reported as
  exactly that — not as "still working."
- **Progress streaming.** A long operation can send advisory progress updates
  ("42% done") that your caller receives as they happen.
- **Real cancellation.** Change your mind and Parley delivers an actual cancel to
  the running handler — not just a local give-up.
- **Errors that tell you what happened.** Every call ends in one typed outcome.
  Alongside your own error codes, six built-in ones distinguish *no one answered*
  from *the pool was full* from *it went silent* from *it timed out* from
  *cancelled* from *the reply didn't match the expected type*.
- **Small enough for tiny devices.** The C++ build is statically allocated, uses
  no exceptions or RTTI, and is driven from your `loop()`. The others carry no
  MQTT-client dependency at all.
- **Bring your own MQTT client.** Parley never opens a connection, never holds
  your broker credentials or TLS keys. You hand it a live connection through a
  tiny adapter you write over whatever client you already use.
- **Typed with your own language's tools.** Request, progress, result, and error
  types are bound with generics / templates / type hints. No schema compiler, no
  code generation.

## How it works

A **command** (say, `backup`) is defined by **four topic strings that you
choose** — Parley imposes no naming scheme, so your broker's access rules stay
entirely in your hands:

| Topic (you name it) | Direction | Carries |
|---|---|---|
| `request` | caller → handler | the request, the result-acknowledgement, and cancel |
| `heartbeat` | handler → caller | "I'm alive and working on it" |
| `progress` | handler → caller | optional progress updates |
| `result` | handler → caller | the single final answer |

Every message is a small JSON object carrying a command id, so many calls can
share the same four topics and each side sorts them out by id. A whole call looks
like this:

```
caller ──req──▶  handler        the request, re-sent until the handler shows life
       ◀──hb───                 heartbeat — the first one means "accepted"
       ◀──prg─── (×N)           progress — advisory, never acknowledged
       ◀──res──                 the single final result — re-sent until acked
       ──rak──▶                 result-acknowledgement stops the re-sends
       (──cnl──▶)               cancel, any time before the result arrives
```

The two roles are a **commander** (the caller) and a **command handler** (the
callee), and they're shaped the same in every language.

The full contract — the state machines, the timers and their defaults, the error
codes, the exact JSON — is the normative spec:
**[`docs/protocol.md`](docs/protocol.md)**.

## A quick look

Here is a whole `backup` command in TypeScript — a handler that serves it and a
commander that calls it. (Python, C#, and C++ versions of this exact scenario are
in [`examples/`](examples/).)

```ts
import {
  ParleyCommander, ParleyCommandHandler, ParleyOutcome, ParleyTopics, type ParleyExecution,
} from 'parley';

// `adapter` is your ParleyConnectionAdapter over an MQTT client — see examples/js/.
const topics = new ParleyTopics(
  'home/backup/req', 'home/backup/hb', 'home/backup/prg', 'home/backup/res',
);

type Req = { target: string };
type Prog = { percent: number };
type Res = { bytesWritten: number };
type Err = 'diskFull' | 'targetUnreachable';

// The handler: perform the work, stream progress, return one typed outcome.
class BackupHandler extends ParleyCommandHandler<Req, Prog, Res, Err> {
  protected async handleCommand(request: Req, execution: ParleyExecution<Prog>) {
    if (execution.signal.aborted) return ParleyOutcome.cancelled();
    execution.reportProgress({ percent: 50 });
    return ParleyOutcome.success({ bytesWritten: 4096 });
  }
}
new BackupHandler(adapter, topics);

// The commander: call it, watch progress, handle the outcome.
const commander = new ParleyCommander<Req, Prog, Res, Err>(adapter, topics);
const result = await commander.execute({ target: 'photos' }, {
  onProgress: (p) => console.log(`${p.percent}%`),
});

if (result.ok) console.log(`wrote ${result.value.bytesWritten} bytes`);
else if (result.error.custom === 'diskFull') console.log('out of space');
else console.log(`failed: ${result.error.code}`);
```

## Using the library

The libraries are **not on package registries yet** (that comes with the 1.0
release — see the roadmap). For now you consume them straight from this
repository. Each detailed example lives in [`examples/`](examples/); a taste of
each idiom follows.

### TypeScript / JavaScript → [`examples/js/`](examples/js/)

Consume from source: add `js/` as a local/workspace dependency (or build it and
point at `js/dist`). Zero runtime dependencies; runs on Node 18+, Deno, Bun, and
edge engines.

```ts
import { ParleyCommander, ParleyTopics } from 'parley';

const topics = new ParleyTopics('home/backup/req', 'home/backup/hb', 'home/backup/prg', 'home/backup/res');
const commander = new ParleyCommander<Req, Prog, Res, Err>(adapter, topics);

const result = await commander.execute({ target: 'photos' }, {
  onProgress: (p) => console.log(`${p.percent}%`),
  signal: AbortSignal.timeout(300_000), // aborting sends a real cancel
});
if (result.ok) console.log(result.value.bytesWritten);
```

### Python → [`examples/python/`](examples/python/)

Consume from source: `pip install ./python`. Pure `asyncio`, zero required
runtime dependencies, fully type-hinted (`py.typed`).

```python
from parley import ParleyCommander, ParleyTopics

topics = ParleyTopics("home/backup/req", "home/backup/hb", "home/backup/prg", "home/backup/res")
commander: ParleyCommander[Req, Prog, Res, Err] = ParleyCommander(adapter, topics)

result = await commander.execute({"target": "photos"}, on_progress=lambda p: print(p["percent"]))
if result.is_success:
    assert result.value is not None
    print(result.value["bytesWritten"])
else:
    assert result.error is not None
    print(result.error.code)
```

### C# / .NET → [`examples/csharp/`](examples/csharp/)

Consume from source: add a project reference to `csharp/src/Parley/Parley.csproj`.
Targets `net10.0` and `netstandard2.0`; trim- and AOT-friendly via
source-generated JSON.

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

### C++ / ESP32-Arduino → [`examples/cpp/`](examples/cpp/)

Consume from source: add `cpp/src` to your include path (Arduino: drop it into
your libraries; PlatformIO: reference the repo). Supply your own ArduinoJson.
Statically allocated, no exceptions/RTTI, tick-driven from `loop()`.

```cpp
// Callbacks are plain function pointers + a void* userData (no std::function).
static void onComplete(void* ud, const ParleyResult<BackupResult, BackupError>& r) {
  if (r.isSuccess()) Serial.printf("wrote %ld bytes\n", r.value().bytesWritten);
  else Serial.printf("failed: %s\n", r.error().code());
}

ParleyCommander<BackupRequest, BackupProgress, BackupResult, BackupError, /*MaxInFlight=*/2>
    commander(adapter, topics);

BackupRequest req; strcpy(req.target, "photos");
ParleyCallHandle handle = commander.execute(req, onComplete, &backupCall);
// in loop():  mqtt.loop();  commander.tick(millis());   // ... commander.cancel(handle) to cancel
```

## Documentation

| Document | What it is |
|---|---|
| [`docs/protocol.md`](docs/protocol.md) | **The wire protocol — normative.** The contract every implementation serves: topics, envelope, state machines, timers, error codes. |
| [`docs/design.md`](docs/design.md) | How that one protocol maps onto the four parallel public APIs. |
| [`docs/testing.md`](docs/testing.md) | The three-layer testing strategy and the language-neutral test artifacts. |
| [`docs/decisions.md`](docs/decisions.md) | Dated log of *why* the design looks the way it does, and what was rejected. |
| [`docs/conformance/`](docs/conformance/) | The shared conformance vectors — 41 JSON scenarios every language replays. |

## Project layout

```
README.md     this file
LICENSE       MIT
docs/         protocol.md (normative) · design.md · testing.md · decisions.md · conformance/
csharp/       C# / .NET reference library (+ tests)
js/           TypeScript / JS reference library (+ tests)
python/       Python / asyncio reference library (+ tests)
cpp/          C++ / ESP32-Arduino reference library (+ tests)
examples/     one worked example per language (the same backup command)
.claude/      working conventions + skills
```

## Testing & conformance

There is **one wire protocol**, and all four implementations are held to it by the
**same 41 conformance vectors** ([`docs/conformance/`](docs/conformance/)) — a
language-neutral suite of scenarios written from the spec text, never adjusted to
match any one implementation. Each library ships a runner that replays every
vector through an in-memory transport on a virtual clock. This shared suite is the
anti-drift net: if two libraries both pass it, they agree on the wire.

On top of that, every library has its own unit tests (including a hard
bounded-state requirement: run thousands of command cycles, then assert every
internal table has returned to its steady-state size). The three-layer strategy is
[`docs/testing.md`](docs/testing.md).

And interop is **verified over the wire**: a dockerized matrix
([`e2e/`](e2e/)) runs all **16 ordered commander×handler language pairs** against
a real mosquitto broker — nine scenarios per pair (payload fidelity incl. a
boundary-sized envelope, progress, cancellation, pool-full `$busy`, a
heartbeats-only silent call) plus orchestrated disruptions: a handler killed
mid-call is detected as `$executionStale` by every caller language, and a broker
restarted mid-call is ridden out through reconnect and retry to a successful
result. A QoS-1 pass covers adapter compatibility. One honest gap remains
recorded: QoS-1 *redelivery deduplication* over a real broker needs persistent
sessions and induced disconnects — future work; the dedup logic itself is
vector-verified in all four libraries.

## Status & roadmap

**Pre-1.0.** All four reference implementations are complete and pass their unit
tests, the shared 41-vector conformance suite, and the full over-the-wire e2e
matrix (16 language pairs, deep disruption scenarios included). Because it's
before 1.0, the public APIs may still change.

Not done yet, roughly in order:

- **The end-to-end matrix** — verified test peers for all four languages talking
  over a real broker (`docs/testing.md`, layer 3).
- **Reference transport adapters** as small packages, so you don't hand-write one
  (the examples' adapters are illustrative for now).
- **Publishing to package registries** (npm, PyPI, NuGet, Arduino/PlatformIO) at
  1.0.

## Security

Parley is deliberately **not** a security layer. It never opens the connection,
and it never sees or stores your broker credentials or TLS material — you connect
and authenticate your own MQTT client, then hand Parley the live session through
the adapter. Authentication and authorization are your **broker's ACLs plus TLS on
your connection**; because you choose every topic string, your existing topic
ACLs apply to Parley's traffic unchanged. Parley also registers no Last-Will and
relies on no retained messages.

## Contributing

Parley is **spec-first**: [`docs/protocol.md`](docs/protocol.md) is the contract,
and the code serves it. Any change to on-the-wire behavior edits the protocol spec
**and** the conformance vectors in the same change, then the four libraries follow.
A vector and an implementation disagreeing is resolved by re-reading the spec — not
by quietly adjusting a vector. Keep the four public APIs deliberately parallel; a
divergence is a design smell to justify in `decisions.md`, not a free choice.

## License

[MIT](LICENSE) © 2026 The Parley Authors.
