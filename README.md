# Parley — request/response RPC over MQTT

[![CI](https://img.shields.io/github/actions/workflow/status/serionist/parley-mqtt/ci.yml?branch=main&event=push&label=CI)](https://github.com/serionist/parley-mqtt/actions/workflows/ci.yml)
[![npm](https://img.shields.io/npm/v/parley-mqtt?label=npm)](https://www.npmjs.com/package/parley-mqtt)
[![PyPI](https://img.shields.io/pypi/v/parley-mqtt?label=PyPI)](https://pypi.org/project/parley-mqtt/)
[![NuGet](https://img.shields.io/nuget/v/Parley.Mqtt?label=NuGet)](https://www.nuget.org/packages/Parley.Mqtt)
[![PlatformIO](https://badges.registry.platformio.org/packages/serionist/library/ParleyMqtt.svg)](https://registry.platformio.org/libraries/serionist/ParleyMqtt)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

**Parley is a small library that gives you two-way RPC — request/reply, command
and control — over the MQTT broker your system already shares**, in four
languages that speak one wire protocol: **C#**, **TypeScript/JS**, **Python** and
**C++ (ESP32/Arduino)**. Publish/subscribe has no built-in notion of *"call this
and wait for the result"*, so people bolt it on by hand — a request topic here, a
reply topic there, a correlation id stuffed in the payload, a timeout guessed at
— and it breaks on exactly the flaky, low-power devices that need it most.

| Ecosystem | Install | Package name |
|---|---|---|
| npm | `npm install parley-mqtt` | `parley-mqtt` |
| PyPI | `pip install parley-mqtt` | `parley-mqtt` (imports as `parley`) |
| NuGet | `dotnet add package Parley.Mqtt` | `Parley.Mqtt` (assembly `Parley`) |
| PlatformIO | `pio pkg install -l "ParleyMqtt"` | `ParleyMqtt` |

What Parley spends its complexity on is **what happens when a call goes wrong**:

- **Liveness, not a stopwatch.** *Nobody answered* (`$noAck`), *it accepted and
  then went silent* (`$executionStale`) and *it ran too long*
  (`$executionTimeout`) are three distinct outcomes, driven by per-execution
  heartbeats — so a slow job is never mistaken for a dead device, and a dead
  device is never reported as "still working".
- **Reliability the protocol earns itself, at QoS 0.** Requests are re-sent until
  the other side shows a sign of life; results are re-sent until acknowledged;
  duplicates are recognized and dropped on both sides, with tombstones. That
  yields at-most-once execution — *at most once per handler process lifetime, for
  as long as the handler retains dedup/tombstone state for that command id* — and
  the residual cases are written down in
  [§9 of the spec](docs/protocol.md#9-reliability-properties-and-residual-caveats),
  not papered over.
- **Cancellation that reaches the running handler.** Changing your mind puts a
  real cancel on the wire and the handler's execution is told, rather than the
  caller quietly giving up locally.
- **The same protocol down to a microcontroller.** The C++ build is statically
  allocated, uses no exceptions or RTTI and is driven from your `loop()`; none of
  the four languages takes a dependency on an MQTT client. Parley needs only MQTT
  3.1.1 ([spec §2](docs/protocol.md#2-transport-assumptions)), so it runs on the
  3.1.1-era client stacks that are still the norm on microcontrollers — no MQTT 5
  broker or client required.

Also here: advisory progress streaming, six built-in error codes alongside your
own typed ones, and no build step — types are bound with your language's own
generics, templates and type hints, so there is no schema compiler and no code
generation. Parley never opens the connection and never holds your broker
credentials or TLS material.

Here is a whole `backup` command in TypeScript — a handler that serves it and a
commander that calls it. (The Python and C# versions of this exact scenario are
in [`examples/`](examples/), and the C++ one ships with the package in
[`cpp/examples/`](cpp/examples/).)

```ts
import {
  ParleyCommander, ParleyCommandHandler, ParleyOutcome, ParleyTopics, type ParleyExecution,
} from 'parley-mqtt';

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

*A "parley" is a conversation between two sides to reach agreement — the shape of
a request and its reply.*

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

The two roles are a **commander** (the caller — the invoker) and a **command
handler** (the callee — the executor), and they're shaped the same in every
language.

The full contract — the state machines, the timers and their defaults, the error
codes, the exact JSON — is the normative spec:
**[`docs/protocol.md`](docs/protocol.md)**.

## Using the library

At **v1.0** the four libraries ship on the standard registries at one lockstep
version each release (the names differ by ecosystem because the bare `parley` was
taken on npm and PyPI — see the install table above). Each detailed example lives
in [`examples/`](examples/); a taste of each idiom follows.

### TypeScript / JavaScript → [`examples/js/`](examples/js/)

`npm install parley-mqtt` — zero runtime dependencies; runs on Node 18+, Deno,
Bun, and edge engines. Ships ESM + CJS + `.d.ts`.

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

### Python → [`examples/python/`](examples/python/)

`pip install parley-mqtt` — then `import parley` (the distribution is
`parley-mqtt`; the module stays `parley`). Pure `asyncio`, zero required runtime
dependencies, fully type-hinted (`py.typed`).

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

`dotnet add package Parley.Mqtt` (the assembly and namespace stay `Parley`).
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

### C++ / ESP32-Arduino → [`cpp/examples/`](cpp/examples/)

**PlatformIO:** add `ParleyMqtt` to your `lib_deps` (or `pio pkg install -l "ParleyMqtt"`);
ArduinoJson is pulled in as a dependency. **Arduino IDE / Library Manager:** not
listed there yet — the Library Manager requires the library at a repo root, so an
automated single-library mirror is planned (see the roadmap); until then drop
`cpp/src` into your `libraries/` folder and supply your own ArduinoJson.
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

## When not to use Parley

Parley is one shape, and it is not the right one for every job. Each case below
states the *need*; follow the pointer and judge for yourself — and see
[`docs/comparison.md`](docs/comparison.md) for what I was able to verify about
each.

- **You need generated client/server stubs from an IDL or schema.** Parley has no
  code generation on purpose; if you want it, the Azure IoT Operations SDKs and
  joynr are worth evaluating.
- **You need several interchangeable workers to share one command queue.** Parley
  pairs one handler with a command's topic set: every handler subscribed to a
  request topic admits and runs the request it sees. Horizontal scaling of
  handlers is a broker-side load-balancing question, and Parley does not answer
  it.
- **You want chunked or streaming transfer.** Parley's progress messages are
  small advisory updates, not a data stream; MQTT+ (TypeScript/JS) and VRPC
  (which proxies callbacks and event emitters) are both worth evaluating.
- **You are already on a platform that ships its own RPC.** If your devices and
  services live inside one vendor's edge platform, its native command mechanism
  will integrate with the rest of that platform in ways an independent library
  cannot.
- **Your whole fleet is MQTT 5 and you want the broker's own request/response.**
  MQTT 5 carries Response Topic and Correlation Data as protocol properties; if
  that is all you need, a thin wrapper over your MQTT client may be enough.

## Related projects

Other work in this space, each described **in its own words** so that a summary
of it is theirs rather than mine. For what I could and could not verify about
each, see [`docs/comparison.md`](docs/comparison.md).

- [**Azure IoT Operations SDKs**](https://github.com/Azure/iot-operations-sdks) —
  describes itself as "SDKs to support the Azure IoT Operations developer
  experience".
- [**joynr**](https://github.com/bmwcarit/joynr) — describes itself as "a
  web-based communication framework for Java, C++, and JavaScript applications
  wanting to interact with other applications, no matter whether they're deployed
  on consumer devices, vehicles, backend infrastructure or in the cloud".
- [**VRPC**](https://github.com/heisenware/vrpc-js) — describes itself as letting
  you "call Node.js, C++, Python, and Arduino classes across any network as if
  they were local objects".
- [**MQTT+**](https://github.com/rse/mqtt-plus) — describes itself as "a
  companion add-on API for the TypeScript/JavaScript API MQTT.js, designed to
  extend MQTT with higher-level communication patterns while preserving full type
  safety".
- [**Sparkplug**](https://github.com/eclipse-sparkplug/sparkplug) — describes
  itself as "a specification for MQTT enabled devices and applications to send and
  receive messages in a stateful way".

## Documentation

| Document | What it is |
|---|---|
| [`docs/protocol.md`](docs/protocol.md) | **The wire protocol — normative.** The contract every implementation serves: topics, envelope, state machines, timers, error codes. |
| [`docs/design.md`](docs/design.md) | How that one protocol maps onto the four parallel public APIs. |
| [`docs/testing.md`](docs/testing.md) | The three-layer testing strategy and the language-neutral test artifacts. |
| [`docs/decisions.md`](docs/decisions.md) | Dated log of *why* the design looks the way it does, and what was rejected. |
| [`docs/conformance/`](docs/conformance/) | The shared conformance vectors — 41 JSON scenarios every language replays. |
| [`docs/comparison.md`](docs/comparison.md) | A dated survey of request/response over MQTT — what I could verify about the alternatives, with sources. |

## Project layout

```
README.md     this file
LICENSE       MIT
docs/         protocol.md (normative) · design.md · testing.md · decisions.md ·
              comparison.md · conformance/
csharp/       C# / .NET reference library (+ tests)
js/           TypeScript / JS reference library (+ tests)
python/       Python / asyncio reference library (+ tests)
cpp/          C++ / ESP32-Arduino reference library (+ tests, + the packaged examples/)
examples/     worked examples for C#, JS/TS and Python (the same backup command;
              the C++ one ships inside the package at cpp/examples/Backup/)
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

**v1.0 — stable.** All four reference implementations are complete and pass their
unit tests, the shared 41-vector conformance suite, and the full over-the-wire
e2e matrix (16 language pairs, deep disruption scenarios included), and ship on
the package registries (npm · PyPI · NuGet · PlatformIO) at one lockstep version.
v1.0 is a semver commitment: the wire protocol is frozen at `v:1` and the public
APIs are stable — a breaking change means a new major version (and, on the wire,
the protocol `v` rules in [`docs/protocol.md`](docs/protocol.md)). One honest gap
stays recorded: QoS-1 redelivery-deduplication over a real broker is not yet
exercised end-to-end (the dedup logic itself is vector-verified in all four
libraries).

On the roadmap:

- **Reference transport adapters** as small packages, so you don't hand-write one
  (the examples' adapters are illustrative for now).
- **Arduino Library Manager** listing via an automated single-library mirror repo
  (PlatformIO ships now; the Library Manager requires a library at the repo root).
- **QoS-1 redelivery-dedup e2e coverage** — needs persistent sessions and induced
  disconnects over a real broker.

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
