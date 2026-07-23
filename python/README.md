# parley-mqtt

**Request/response RPC over MQTT** — the Python (asyncio) reference implementation of the Parley v1 protocol.

> Install `parley-mqtt`; import `parley`. (The bare `parley` distribution name on PyPI is an
> unrelated project; the module name is unchanged.)

Parley turns the MQTT broker you already share into something you can make *requests* over:
call a command on another device, stream progress while it runs, cancel it, and receive one
typed result — success or a clear error. It is a small library, not a server: it rides your
existing MQTT connection and never holds your broker credentials or TLS material.

One wire protocol, four parallel reference libraries — **C#**, **TypeScript/JS**, **Python**, and
**C++ (ESP32/Arduino)** — so a .NET service, an edge script, a Python service, and firmware can all
call each other. `parley-mqtt` is the Python one.

- Pure `asyncio`; fully type-hinted (`py.typed`); CPython 3.10+.
- Zero required runtime dependencies — you supply a live MQTT connection through a tiny adapter
  you write.
- At-most-once execution correct even at QoS 0; retries, dedup, heartbeats, progress, real
  cancellation, and six built-in error codes.

```bash
pip install parley-mqtt
```

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
