# Where Parley sits — a survey of request/response over MQTT (2026-07-23)

This page exists so you can decide quickly whether Parley is the right tool, and reach the right
alternative fast if it isn't.

**Read it as a snapshot, not a verdict.** Everything below is **what I could verify from public
documentation on 2026-07-23** — the projects' own specs, READMEs and repositories, linked from
every non-obvious cell. I have not run any of these systems, and I have not audited their client
source. Where a capability simply isn't described in a project's own documentation, the table says
**"not documented"** — that means *I could not find it written down*, not *the project cannot do
it*. Several of these are large projects and I will have missed things.

**Corrections are very welcome.** If a cell is wrong, out of date, or unfair to your project, please
[open an issue](https://github.com/serionist/parley-mqtt/issues) and I will fix it — this file is in
the repository precisely so it can be corrected, rather than baked into a registry page that can
never be edited.

## The table

| Project | Languages it documents | MQTT version | IDL / codegen | Liveness of an already-accepted call | Cancellation on the wire | Progress / streaming | Runs on an MCU |
|---|---|---|---|---|---|---|---|
| **Parley** | C#, TypeScript/JS, Python, C++ ([`README.md`](../README.md)) | 3.1.1 or later; uses no MQTT 5 features ([spec §2](protocol.md#2-transport-assumptions)) | none — types bound with each language's own generics/templates/hints | per-execution heartbeats; `$noAck`, `$executionStale` and `$executionTimeout` are three distinct outcomes ([spec §5](protocol.md#5-error-codes)) | yes — `cnl` on the request topic, delivered to the running handler ([spec §6, C7](protocol.md#6-commander-behavior)) | advisory `prg` messages, never acknowledged ([spec §7](protocol.md#7-handler-behavior)) | yes — statically allocated C++ core, no exceptions/RTTI |
| **MQTT 5 request/response** (the broker feature itself) | n/a — a protocol feature, not a library | **MQTT 5 required** — Response Topic and Correlation Data are v5 properties (§3.3.2.3.5 and §3.3.2.3.6 on a PUBLISH, per [§4.10][mqtt5-rr]) | n/a | out of scope — §4.10 defines three properties and leaves the interaction to the application ([§4.10][mqtt5-rr]) | out of scope — as above ([§4.10][mqtt5-rr]) | out of scope — as above ([§4.10][mqtt5-rr]) | depends entirely on your client stack |
| **Azure IoT Operations SDKs** | .NET, Go, Rust ([README][aio]) | QoS 1 with persistent sessions ([RPC protocol][aio-rpc]) | yes — a protocol compiler generates client libraries and server stubs from a DTDL model ([components][aio-comp]) | not documented in the RPC protocol reference ([RPC protocol][aio-rpc]) | not documented in the RPC protocol reference ([RPC protocol][aio-rpc]) | documented as not yet included: *"It does not including streaming, sending commands to multiple receivers, etc. Those will be added soon."* ([RPC protocol][aio-rpc]) | not documented |
| **joynr** | Java, C++, JavaScript ([README][joynr]) | transport-agnostic — MQTT is one of several transports ([README][joynr]) | yes — Franca IDL models and a code generator ([README][joynr]) | not documented in the material I read ([README][joynr]) | not documented in the material I read ([README][joynr]) | not documented in the material I read ([README][joynr]) | not documented |
| **VRPC** | Node.js, C++, Python, Arduino ([README][vrpc]) | *"They use the MQTT 3.11 standard"* ([protocol reference][vrpc-proto]) | none — it adapts existing classes directly ([README][vrpc]) | **not documented in its protocol reference; client source not audited** ([protocol reference][vrpc-proto]) | **not documented in its protocol reference; client source not audited** ([protocol reference][vrpc-proto]) | yes — continuous callbacks/events are encoded on the wire (`__f__<method>-<index>-<event\|invokeId>`) ([protocol reference][vrpc-proto]); the README describes proxying `EventEmitter` instances ([README][vrpc]) | an Arduino implementation exists ([vrpc-arduino][vrpc-arduino]) |
| **MQTT+** | TypeScript/JavaScript, as an add-on to MQTT.js ([README][mqttplus]) | not documented in the material I read ([README][mqttplus]) | none — TypeScript types ([README][mqttplus]) | not documented in the material I read ([README][mqttplus]) | not documented in the material I read ([README][mqttplus]) | yes — it lists stream-based sink-push and source-fetch patterns alongside RPC-style service calls ([README][mqttplus]) | not documented |
| **Sparkplug** | specification + TCK; many independent implementations ([repository][sparkplug]) | — *(state/telemetry, not RPC; listed because it comes up in the same searches)* | metric definitions, not RPC stubs | device/application birth-death lifecycle ([repository][sparkplug]) | n/a | n/a | widely implemented on devices |

A blunt reading of that table: **Parley's shape is a small, dependency-free protocol library that
gives one wire contract to four languages including a microcontroller, and that spends its
complexity budget on what happens when a call goes wrong** — liveness, cancellation and dedup —
rather than on generating code from a schema. If you want generated stubs from an IDL, or you are
already inside a platform that ships its own RPC, that is a different and entirely reasonable set of
priorities, and the projects above serve it.

## On conformance testing — not a category first

Parley ships [41 language-neutral conformance vectors](conformance/) that every implementation
replays, plus a [16-pair over-the-wire interop matrix](testing.md). This discipline is **not
unique**, and it would be misleading to present it that way:

- **Azure IoT Operations also ships shared, cross-language protocol test cases.** They live outside
  any one language's tree, as 181 YAML cases (of 185 files) under
  [`eng/test/test-cases/Protocol/`][aio-cases] (CommandInvoker, CommandExecutor, TelemetrySender,
  TelemetryReceiver), written in a documented DSL — *"METL (MQTT Envoy Testing Language)
  Specification. This document describes the format of the domain-specific language METL, in which
  Akri.Mqtt unit tests are written"* ([METL spec][aio-metl]) — and the testing notes state that the
  *"normative definion of tests aspires to be abstract and not assume any language specific
  constructs"* ([RPC protocol testing][aio-testing]).
- **Sparkplug has a full TCK** — a Technology Compatibility Kit maintained alongside the
  specification, in [`tck/`][sparkplug-tck].

What is true of Parley is narrower and worth stating precisely: the same vector suite is replayed by
**four** implementations including a **statically-allocated C++ core**, and the interop matrix
exercises **all 16 ordered commander × handler language pairs against a real broker**. That is the
same discipline, applied at this project's particular spread.

## Sources

Every link below was fetched and confirmed to resolve on 2026-07-23, and each project was checked
and found **not archived** on that date.

- **MQTT v5.0** — OASIS Standard. [Specification][mqtt5]. Request/Response is §4.10: *"Some
  applications or standards might wish to run a Request/Response interaction over MQTT. This version
  of MQTT includes three properties that can be used for this purpose"* ([§4.10][mqtt5-rr]);
  On a PUBLISH these are §3.3.2.3.5 (Response Topic) and §3.3.2.3.6 (Correlation Data), as
  [§4.10][mqtt5-rr] itself cross-references; §3.1.3.2.6/§3.1.3.2.7 are the corresponding *Will*
  properties, which is a different thing.
- **Azure IoT Operations SDKs** — describes itself as *"SDKs to support the Azure IoT Operations
  developer experience"* ([README][aio]). The protocol compiler is *"a command line tool distributed
  as a NuGet package. It generates client libraries and server stubs in multiple languages from a
  DTDL input"*, and commands *"Implement an RPC pattern, to decouple clients and servers, where the
  client invokes the command, and the server executes the command"* ([components][aio-comp]).
  *"Invoker and executor use Persistent sessions and QoS1 for the MQTT communications"*
  ([RPC protocol][aio-rpc]).
- **joynr** — describes itself as *"a web-based communication framework for Java, C++, and
  JavaScript applications wanting to interact with other applications, no matter whether they're
  deployed on consumer devices, vehicles, backend infrastructure or in the cloud"* ([README][joynr]);
  the repository summary calls it *"A transport protocol agnostic (MQTT, HTTP, WebSockets etc.)
  Franca IDL based communication framework"*.
- **VRPC** — describes itself as: *"Stop writing API boilerplate. VRPC (Virtual Remote Procedure
  Call) allows you to call Node.js, C++, Python, and Arduino classes across any network as if they
  were local objects"* ([README][vrpc]). Its protocol reference states *"All VRPC agents and clients
  are MQTT clients to a single logical broker. They use the MQTT 3.11 standard to realize all RPC
  functionality"* ([protocol reference][vrpc-proto]). **The retry, liveness and cancellation cells
  for VRPC say "not documented in its protocol reference" because that is exactly
  what I checked — I did not audit the client source, and the behavior may well exist in code.**
  The Arduino implementation is a separate repository ([vrpc-arduino][vrpc-arduino]).
- **MQTT+** — describes itself as *"a companion add-on API for the TypeScript/JavaScript API MQTT.js,
  designed to extend MQTT with higher-level communication patterns while preserving full type
  safety"*, listing fire-and-forget event emission, RPC-style service call, stream-based sink push
  and stream-based source fetch ([README][mqttplus]).
- **Sparkplug** — describes itself as *"a specification for MQTT enabled devices and applications to
  send and receive messages in a stateful way"* ([repository][sparkplug]); the TCK is maintained in
  the same repository ([`tck/`][sparkplug-tck]).
- **PubSubClient** — an MQTT client much Arduino/ESP32 material reaches for. Its maintainer marked
  it unmaintained in 2026, which does not change the version point below. Its README states:
  *"The client uses MQTT 3.1.1 by default. It can be changed to use MQTT 3.1 by changing value of
  `MQTT_VERSION` in `PubSubClient.h`"* ([README][pubsubclient]). This is the concrete reason
  Parley's "MQTT 3.1.1 is enough" property matters on a microcontroller: a request/response design
  that needs MQTT 5 properties cannot run on that client at all.

[mqtt5]: https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html
[mqtt5-rr]: https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html#_Toc464564287
[aio]: https://github.com/Azure/iot-operations-sdks
[aio-comp]: https://github.com/Azure/iot-operations-sdks/blob/main/doc/components.md
[aio-rpc]: https://github.com/Azure/iot-operations-sdks/blob/main/doc/reference/rpc-protocol.md
[aio-cases]: https://github.com/Azure/iot-operations-sdks/tree/main/eng/test/test-cases/Protocol
[aio-metl]: https://github.com/Azure/iot-operations-sdks/blob/main/doc/dev/generated/MetlSpec.md
[aio-testing]: https://github.com/Azure/iot-operations-sdks/blob/main/doc/dev/testing/rpc-protocol-testing.md
[joynr]: https://github.com/bmwcarit/joynr
[vrpc]: https://github.com/heisenware/vrpc-js
[vrpc-proto]: https://github.com/heisenware/vrpc-js/blob/master/docs/protocol.md
[vrpc-arduino]: https://github.com/heisenware/vrpc-arduino
[mqttplus]: https://github.com/rse/mqtt-plus
[sparkplug]: https://github.com/eclipse-sparkplug/sparkplug
[sparkplug-tck]: https://github.com/eclipse-sparkplug/sparkplug/tree/master/tck
[pubsubclient]: https://github.com/knolleary/pubsubclient
