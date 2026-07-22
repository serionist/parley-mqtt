# Parley — cross-language library design

**Status: non-normative.** The wire contract is [`protocol.md`](protocol.md); this document maps
it onto the four public APIs. The concepts and call sequences are deliberately parallel across
C#, Python, JS/TS, and C++ — names follow each ecosystem's idiom, but a structural divergence is
a design smell to justify in [`decisions.md`](decisions.md), not a free choice.

## Concepts (identical in all four languages)

| Concept | Role |
|---|---|
| `ParleyCommander` | caller side of one command type; generic over request, progress, result, and custom-error types |
| `ParleyCommandHandler` | abstract callee side of one command type; the consumer overrides the handle method |
| `ParleyConnectionAdapter` | abstract transport port the consumer implements over their own MQTT client |
| `ParleyTopics` | the four topic strings: request, heartbeat, progress, result |
| `ParleyConfig` | timing configuration (protocol.md §8) + the handler caps `maxConcurrentExecutions` / `maxActiveCommands` |
| `ParleyCallOptions` | per-call overrides: commander-local timeouts (+ the progress sink where idiomatic) |
| `ParleyResult` | terminal outcome: success with the result payload, or an error (builtin or custom code) |

Construction is uniform: `(adapter, topics, config)` plus the type bindings each language needs,
and two uniform injectable seams — the command-id generator (protocol.md §4.2) and the clock —
defaulted in production, overridden by the test suites. Commander and handler each own their
subscriptions through the adapter and re-subscribe on its connected signal. Expected outcomes
(all builtin codes, custom errors) are returned as values — never thrown/rejected across the API
boundary; exceptions are reserved for programming errors (bad config, misuse).

Cross-language contracts settled by the reference implementation:

- **Custom errors are string codes.** A `TError` maps to the wire `e` string; it must serialize
  to a plain JSON string, and `$`-prefixed values are rejected fail-fast (§5). The optional `m`
  detail surfaces as `Error.Message` alongside `Error.Code` (the raw wire string). Structured
  error `p` remains wire-legal (§4.1) but is not surfaced by the v1 APIs.
- **Instances are disposable.** Disposing a commander/handler detaches its adapter subscriptions
  and the connected callback, and completes any in-flight calls locally with `$cancelled`.
- **Adapter `subscribe` replaces.** `subscribe(topic, onMessage)` replaces any prior handler for
  that topic (one handler per topic) — the library re-subscribes on every connected signal
  without unsubscribing first; an appending adapter would multiply-deliver.
- **Synchronous delivery is legal; publish comes last.** An adapter MAY deliver messages
  synchronously from inside `publish` (in-process loopback). Implementations therefore complete
  ALL state mutation and timer scheduling for a transition BEFORE invoking `adapter.publish` —
  a re-entrant reply must find the transition already consistent.
- **Fire the connected callback OFF the client's connect call stack.** Some MQTT clients raise
  their connected event synchronously inside their own connect call (MQTTnet does). The
  library's connected callback re-subscribes — which calls back into the client — so an adapter
  that forwards the event inline can re-enter the client's connect path and deadlock while the
  library holds its dispatch gate. Adapters defer the forward (task/queue/next-tick). Found
  live by the e2e broker-restart scenario.
- **Thrown cancellation maps to `$cancelled`.** A handler that unwinds via a cancellation throw
  while its execution signal is aborted (`OperationCanceledException`, `AbortError`, …) yields
  the same `$cancelled` result as one that returns a cancelled outcome; only a non-cancellation
  exception aborts silently.
- **Codec hooks (optional, pass-through by default).** Commander: `encodeRequest`,
  `decodeResult`, `decodeProgress`; handler: `decodeRequest`, `encodeResult`, `encodeProgress`.
  A throwing `decodeResult` yields `$decodeError` (rak still sent); a throwing `decodeRequest`
  silently drops the request; encode-side throws abort like a thrown handler. In statically
  typed languages the codec is the type binding itself (C#: `JsonTypeInfo`); in JS it is the
  optional validation seam. A codec may see "no payload" (absent/null `p`) — it should treat
  that as its language's empty value.
- **Commander in-flight is timer-bounded, not pool-bounded, in the managed languages.** Every
  call carries a guaranteed terminal timer, so in-flight state drains by construction; C++'s
  `MaxInFlight` pool is a static-allocation necessity, not a protocol cap.
- **Handlers should yield early.** The synchronous prefix of the handle method runs on the
  library's dispatch path; heavy synchronous work delays other commands' heartbeats.

## The transport adapter

One abstract class per language; the consumer implements it against MQTTnet, paho-mqtt, mqtt.js,
PubSubClient, or anything else. The core never references a concrete client.

```
abstract ParleyConnectionAdapter
  publish(topic, payloadBytes)              // fire-and-forget; retain MUST be false
  subscribe(topic, onMessage)               // onMessage(topic, payloadBytes)
  unsubscribe(topic)
  onConnected(callback)                     // fired on every (re)connection; the library
                                            // re-subscribes its topics in this callback
```

The adapter chooses QoS 0 or 1 (protocol is correct at 0). Reference adapters ship as separate
packages/examples, never as core dependencies. Multiple commanders/handlers may share one
underlying client; the simplest shape is one adapter instance per Parley object wrapping the
shared client.

## C\#

```csharp
var commander = new ParleyCommander<BackupRequest, BackupProgress, BackupResult, BackupError>(
    adapter, topics, config, jsonContext);          // System.Text.Json source-gen context

ParleyResult<BackupResult, BackupError> result = await commander.ExecuteAsync(
    new BackupRequest(...),
    new ParleyCallOptions { ExecutionTimeout = TimeSpan.FromMinutes(10) },
    progress: new Progress<BackupProgress>(p => ...),
    cancellationToken: ct);                          // cancellation → wire-level cnl

if (result.IsSuccess) Use(result.Value);
else Handle(result.Error);                           // Error.IsBuiltin / Error.Builtin / Error.Custom

public sealed class BackupHandler
    : ParleyCommandHandler<BackupRequest, BackupProgress, BackupResult, BackupError>
{
    protected override async Task<ParleyOutcome<BackupResult, BackupError>> HandleCommandAsync(
        BackupRequest request, IParleyExecution<BackupProgress> execution, CancellationToken ct)
    {
        execution.ReportProgress(new BackupProgress(...));
        return ParleyOutcome.Success(new BackupResult(...));   // or ParleyOutcome.Error(code)
    }
}
```

Conventions that apply (see `.claude/CONVENTIONS.md`): `netstandard2.0` + current `net*`
multi-target, source-generated serialization only, `CancellationToken` threaded everywhere, no
`async void`, core package free of any MQTT client reference.

## Python

```python
commander = ParleyCommander[BackupRequest, BackupProgress, BackupResult, BackupError](
    adapter, topics, config, codec)

result = await commander.execute(
    BackupRequest(...),
    options=ParleyCallOptions(execution_timeout=600.0),   # seconds; None inherits, math.inf disables
    on_progress=lambda p: ...,
    signal=cancel_signal)   # ParleyCancelSignal: .aborted + a SYNCHRONOUS fire-on-abort listener —
                            # never task-cancellation of execute() (outcomes are values).
                            # asyncio.Event/Future do not satisfy it; an Event adapter ships.

if result.is_success: use(result.value)
else: handle(result.error)

class BackupHandler(ParleyCommandHandler[BackupRequest, BackupProgress, BackupResult, BackupError]):
    async def handle_command(
        self, request: BackupRequest, execution: ParleyExecution[BackupProgress]
    ) -> ParleyOutcome[BackupResult, BackupError]:
        execution.report_progress(BackupProgress(...))
        return ParleyOutcome.success(BackupResult(...))
```

asyncio-first; a wire-level `cnl` cancels the running `handle_command` task
(`asyncio.CancelledError` — handlers clean up with the normal `try/finally` idiom). Fully typed
(`py.typed`); zero required runtime deps in core; the codec maps dataclasses ↔ JSON dicts.

## JS / TypeScript

```ts
const commander = new ParleyCommander<BackupRequest, BackupProgress, BackupResult, BackupError>(
  adapter, topics, config, { codec, idGenerator, clock });   // options object; all optional

const result = await commander.execute(request, {
  executionTimeout: 600_000,
  onProgress: (p) => { ... },
  signal: abortController.signal,          // abort → wire-level cnl
});

if (result.ok) use(result.value);
else handle(result.error);                 // discriminated union; never rejects for expected outcomes

class BackupHandler extends ParleyCommandHandler<BackupRequest, BackupProgress, BackupResult, BackupError> {
  protected async handleCommand(
    request: BackupRequest, execution: ParleyExecution<BackupProgress>
  ): Promise<ParleyOutcome<BackupResult, BackupError>> {
    execution.reportProgress({ ... });
    if (execution.signal.aborted) return ParleyOutcome.cancelled();
    return ParleyOutcome.success({ ... });
  }
}
```

TypeScript strict; ESM + CJS + `.d.ts`; zero runtime deps in core. The per-call progress callback
and `AbortSignal` live in the options object (the `ParleyCallOptions` analogue).

## C++ (ESP32/Arduino-first)

No exceptions, no heap churn: template parameters carry the types **and the pool sizes**; all
state is statically allocated; everything is pumped from `loop()` — no blocking waits.

```cpp
ParleyCommander<BackupRequest, BackupProgress, BackupResult, BackupError,
                /*MaxInFlight=*/2> commander(adapter, topics, config);

auto callHandle = commander.execute(request, opts,
                                    onProgress /*fn ptr*/, onComplete /*fn ptr*/, userData /*void**/);
commander.cancel(callHandle);            // → wire-level cnl

class BackupHandler : public ParleyCommandHandler<BackupRequest, BackupProgress, BackupResult,
                                                  BackupError, /*MaxCommands=*/2> {
  void handleCommand(ParleyExecution<BackupRequest, BackupProgress, BackupResult,
                                     BackupError>& exec) override {
    const BackupRequest& request = exec.request();
    // long work stays non-blocking: stash &exec, keep working across loop() turns, then:
    exec.reportProgress(progress);
    if (exec.isCancelRequested()) { exec.cancel(); return; }
    exec.succeed(result);                // or exec.fail(code)
  }
};

void loop() { commander.tick(millis()); handler.tick(millis()); }
```

ArduinoJson codec; always the explicit-length publish overload; a full pool answers `$busy` per
protocol.md H2, which is exactly why the pools can stay tiny (the `MaxCommands` template parameter
supplies `maxActiveCommands`).

Static-allocation consequences (C++-only; wire-relevant where noted):

- **Outbound-only adapter.** The C++ adapter carries `publish` (explicit length) +
  `subscribe`/`unsubscribe` intent; inbound delivery, the connected notification, and
  `tick(nowMs)` are methods on the commander/handler — no stored callback closures on a
  no-heap target.
- **`MaxPayload` bounds both wire directions** (defaulted template parameter): outbound
  wire-byte caches AND inbound parse capacity. An inbound message longer than `MaxPayload` is
  unparseable and silently dropped per protocol.md §4 — residuals: an oversize `req` ends in
  the caller's `$noAck`; an oversize `res` lets the call reach its timeout. `MaxTombstones`
  (defaulted, pool×4) statically sizes the tombstone table.
- **Commander pool exhaustion is local.** A full `MaxInFlight` pool makes `execute()` fail
  synchronously with an invalid-handle local error — never a `$` wire code.
- **`kNoTimeout` disables the execution cap** via an explicit skip-scheduling branch — never
  `start + kNoTimeout` arithmetic (uint32 wraparound) — parity with Python's `math.inf` and
  TS's `null`. A native desktop build runs the
same core against the conformance vectors without hardware.

## Where the logic lives

Fat core, thin edges: correlation, retry, dedup, tombstones, heartbeating, timeouts, and the
state machines of protocol.md §6–§7 are implemented once per language in a transport-free core,
tested against the shared conformance vectors over a fake transport. The adapter translates
publish/subscribe/connected — nothing else. Codecs translate typed payloads ↔ envelope JSON —
nothing else.
