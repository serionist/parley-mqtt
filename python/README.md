# parley-mqtt

[![CI](https://img.shields.io/github/actions/workflow/status/serionist/parley-mqtt/ci.yml?branch=main&event=push&label=CI)](https://github.com/serionist/parley-mqtt/actions/workflows/ci.yml)
[![PyPI](https://img.shields.io/pypi/v/parley-mqtt?label=PyPI)](https://pypi.org/project/parley-mqtt/)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://github.com/serionist/parley-mqtt/blob/main/LICENSE)

**Request/response RPC over MQTT** — the Python (asyncio) reference implementation of the Parley v1
protocol.

```bash
pip install parley-mqtt
```

> Install `parley-mqtt`; import `parley`. (The bare `parley` distribution name on PyPI is an
> unrelated project; the module name is unchanged.)

Parley turns the MQTT broker you already share into something you can make *requests* over —
request/reply, not just publish/subscribe. Call a command on another device or service, stream
progress while it runs, cancel it, and receive one typed result: success or a clear error. Each
command is four topic strings you choose, and every message carries a correlation id, so many
calls share the same topics.

One wire protocol, four parallel reference libraries — **Python**, **C#**, **TypeScript/JS** and
**C++ (ESP32/Arduino)** — so a Python service, a .NET service, an edge script and firmware can all
call each other over the broker they already share. `parley-mqtt` is the Python one. It is a small
library, not a server: it rides your existing MQTT connection and never holds your broker
credentials or TLS material.

- **Pure `asyncio`, fully type-hinted (`py.typed`), CPython 3.10+.** Request, progress, result and
  error types are bound with ordinary type hints — no schema compiler, no code generation, no build
  step.
- **Zero required runtime dependencies, no MQTT-client dependency.** You supply a live connection
  through a tiny adapter you write over paho-mqtt or whatever you already use, and Parley needs only
  MQTT 3.1.1 — no MQTT 5 broker required.
- **Three distinct liveness outcomes.** *Nobody answered* (`$noAck`), *it accepted and then went
  silent* (`$executionStale`) and *it ran too long* (`$executionTimeout`) are separate results,
  driven by per-execution heartbeats rather than one guessed stopwatch.
- **At-most-once execution, correct even at QoS 0.** Requests are re-sent until the other side
  shows a sign of life, results are re-sent until acknowledged, and duplicates are recognized and
  dropped on both sides. The guarantee is *at most once per handler process lifetime, for as long
  as the handler retains dedup/tombstone state for that command id* — the residual cases are
  written down in
  [§9 of the protocol spec](https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md#9-reliability-properties-and-residual-caveats).
- **Cancellation that reaches the running handler** — cancelling puts a real cancel on the wire, not
  a local give-up. Plus advisory progress streaming and six built-in error codes alongside your own.

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

## Documentation

- **Repository:** <https://github.com/serionist/parley-mqtt>
- **The wire protocol (normative):** <https://github.com/serionist/parley-mqtt/blob/main/docs/protocol.md>
- **Worked Python example:** <https://github.com/serionist/parley-mqtt/tree/main/examples/python>
- **Design notes:** <https://github.com/serionist/parley-mqtt/blob/main/docs/design.md>

## License

[MIT](https://github.com/serionist/parley-mqtt/blob/main/LICENSE) © The Parley Authors.
