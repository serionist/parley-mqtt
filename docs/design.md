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
| `ParleyConfig` | timing configuration (protocol.md §8) + `maxConcurrentExecutions` on the handler side |
| `ParleyCallOptions` | per-call overrides: commander-local timeouts (+ the progress sink where idiomatic) |
| `ParleyResult` | terminal outcome: success with the result payload, or an error (builtin or custom code) |

Construction is uniform: `(adapter, topics, config)` plus the type bindings each language needs.
Commander and handler each own their subscriptions through the adapter and re-subscribe on its
connected signal. Expected outcomes (all five builtin codes, custom errors) are returned as
values — never thrown/rejected across the API boundary; exceptions are reserved for programming
errors (bad config, misuse).

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
    options=ParleyCallOptions(execution_timeout=600.0),
    on_progress=lambda p: ...)

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
  adapter, topics, config, codec);

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
protocol.md H2, which is exactly why the pools can stay tiny. A native desktop build runs the
same core against the conformance vectors without hardware.

## Where the logic lives

Fat core, thin edges: correlation, retry, dedup, tombstones, heartbeating, timeouts, and the
state machines of protocol.md §6–§7 are implemented once per language in a transport-free core,
tested against the shared conformance vectors over a fake transport. The adapter translates
publish/subscribe/connected — nothing else. Codecs translate typed payloads ↔ envelope JSON —
nothing else.
