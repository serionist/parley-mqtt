"""Parley layer-3 interop test peer (Python / asyncio), a GENERIC interpreter of
docs/conformance/interop/scenarios.json over the real paho-mqtt client.

Handler role serves the five fixed harness command behaviours and prints READY after all SUBACKs;
commander role runs the step model, writes the JSON report, and exits nonzero on any failure. Zero
suite logic lives here — only the harness. QoS 0, retain=False (protocol.md §2 baseline).

paho runs its network loop on a background thread; every inbound callback is marshalled back onto the
asyncio loop with ``call_soon_threadsafe`` (the marshaling the examples session validated). Durations
in scenarios.json are milliseconds; the Python core takes seconds, so each is divided by 1000.
"""

from __future__ import annotations

import asyncio
import json
import sys
from typing import Any

import paho.mqtt.client as mqtt

from parley import (
    ParleyCancelSignal,
    ParleyCommander,
    ParleyCommandHandler,
    ParleyConfig,
    ParleyConnectionAdapter,
    ParleyExecution,
    ParleyMessageHandler,
    ParleyOutcome,
    ParleyTopics,
)

SELF_LANG = "python"
READY_LINE = "PARLEY-PEER-READY"
COMMANDS = ["echo", "slow", "err", "cancelme", "busy1"]


# ---------------------------------------------------------------------------
# Real transport adapter over paho-mqtt, with SUBACK tracking so READY / suite
# start gate on the broker actually holding the subscriptions (QoS 0 has no
# retained state — publishing before a subscription lands would drop silently).
# ---------------------------------------------------------------------------
class PahoAdapter(ParleyConnectionAdapter):
    def __init__(self, client: mqtt.Client, loop: asyncio.AbstractEventLoop, qos: int = 0) -> None:
        super().__init__()
        self._client = client
        self._loop = loop
        self._qos = qos
        self._handlers: dict[str, ParleyMessageHandler] = {}
        self._pending_mids: set[int] = set()
        client.on_message = self._on_message
        client.on_connect = self._on_connect
        client.on_subscribe = self._on_subscribe

    def publish(self, topic: str, payload: bytes) -> None:
        self._client.publish(topic, payload, qos=self._qos, retain=False)

    def subscribe(self, topic: str, on_message: ParleyMessageHandler) -> None:
        self._handlers[topic] = on_message
        if self._client.is_connected():
            _result, mid = self._client.subscribe(topic, qos=self._qos)
            if mid is not None:
                self._pending_mids.add(mid)

    def unsubscribe(self, topic: str) -> None:
        self._handlers.pop(topic, None)
        if self._client.is_connected():
            self._client.unsubscribe(topic)

    def _on_connect(self, _client: object, _userdata: object, _flags: object,
                    _reason: object, _props: object = None) -> None:
        self._loop.call_soon_threadsafe(self.raise_connected)

    def _on_message(self, _client: object, _userdata: object, message: mqtt.MQTTMessage) -> None:
        handler = self._handlers.get(message.topic)
        if handler is not None:
            self._loop.call_soon_threadsafe(handler, message.topic, message.payload)

    def _on_subscribe(self, _client: object, _userdata: object, mid: int,
                      _reason_codes: object, _props: object = None) -> None:
        self._loop.call_soon_threadsafe(self._pending_mids.discard, mid)

    async def wait_for_subscriptions(self, timeout: float = 10.0) -> None:
        deadline = self._loop.time() + timeout
        while self._pending_mids and self._loop.time() < deadline:
            await asyncio.sleep(0.02)


# ---------------------------------------------------------------------------
# The harness handler — the five fixed command behaviours.
# ---------------------------------------------------------------------------
class HarnessHandler(ParleyCommandHandler[Any, Any, Any, str]):
    def __init__(self, adapter: ParleyConnectionAdapter, topics: ParleyTopics,
                 config: ParleyConfig, kind: str) -> None:
        super().__init__(adapter, topics, config)
        self._kind = kind

    async def handle_command(
        self, request: Any, execution: ParleyExecution[Any]
    ) -> ParleyOutcome[Any, str]:
        kind = self._kind
        if kind == "echo":
            return ParleyOutcome.success(request)
        if kind == "err":
            return ParleyOutcome.error(str(get_field(request, "code", "error")))
        if kind == "slow":
            duration = num_field(request, "durationMs", 1000)
            every = max(1, num_field(request, "progressEveryMs", 200))
            execution.report_progress({"i": 0})
            elapsed = 0
            i = 0
            while elapsed < duration:
                await asyncio.sleep(every / 1000)
                elapsed += every
                i += 1
                execution.report_progress({"i": i})
            return ParleyOutcome.success({"done": True})
        # cancelme / busy1: park while emitting progress until a wire cnl cancels the task.
        every = max(1, num_field(request, "progressEveryMs", 200))
        execution.report_progress({"i": 0})
        i = 0
        while not execution.signal.aborted:
            await asyncio.sleep(every / 1000)
            i += 1
            execution.report_progress({"i": i})
        return ParleyOutcome.cancelled()


# ---------------------------------------------------------------------------
# The commander step interpreter.
# ---------------------------------------------------------------------------
class PeerAssert(Exception):
    pass


def start_call(call: dict, commanders: dict) -> dict:
    command = call["command"]
    commander = commanders.get(command)
    if commander is None:
        raise PeerAssert(f"unknown command '{command}'")
    signal = ParleyCancelSignal()
    rec: dict = {"command": command, "signal": signal, "progress": 0}

    def on_progress(_p: object) -> None:
        rec["progress"] += 1

    rec["task"] = asyncio.ensure_future(
        commander.execute(call.get("p"), on_progress=on_progress, signal=signal)
    )
    return rec


def require_call(named: dict, ref: dict) -> dict:
    rec = named.get(ref["name"])
    if rec is None:
        raise PeerAssert(f"reference to unknown call '{ref['name']}'")
    return rec


async def barrier(scenario: str, rec: dict, barrier_ms: int) -> None:
    loop = asyncio.get_running_loop()
    start = loop.time()
    while loop.time() - start < barrier_ms / 1000:
        if rec["progress"] >= 1:
            return
        if rec["task"].done():
            raise PeerAssert(f"{scenario}: awaitProgress '{rec['command']}' — terminal outcome before any progress")
        await asyncio.sleep(0.015)
    raise PeerAssert(f"{scenario}: awaitProgress '{rec['command']}' — no progress within {barrier_ms}ms")


async def await_cap(rec: dict, call_ms: int) -> Any:
    task = rec["task"]
    try:
        await asyncio.wait_for(asyncio.shield(task), timeout=call_ms / 1000)
    except asyncio.TimeoutError as exc:
        raise PeerAssert(f"call '{rec['command']}' — no terminal outcome within {call_ms}ms") from exc
    return task.result()


def check_expect(scenario: str, expect: dict, res: Any, rec: dict) -> None:
    if expect["ok"]:
        if not res.is_success:
            raise PeerAssert(f"{scenario}: expected success, got error '{res.error.code}'")
        if "p" in expect and not deep_equal(res.value, expect["p"]):
            raise PeerAssert(
                f"{scenario}: result payload mismatch. expected {json.dumps(expect['p'])}, got {json.dumps(res.value)}"
            )
    else:
        if res.is_success:
            raise PeerAssert(f"{scenario}: expected error but succeeded")
        if "e" in expect and res.error.code != expect["e"]:
            raise PeerAssert(f"{scenario}: error code mismatch. expected '{expect['e']}', got '{res.error.code}'")
    if "minProgress" in expect and rec["progress"] < expect["minProgress"]:
        raise PeerAssert(f"{scenario}: minProgress not met. expected >= {expect['minProgress']}, got {rec['progress']}")


async def run_scenario(scenario: dict, commanders: dict, barrier_ms: int, call_ms: int) -> dict:
    name = scenario["name"]
    named: dict = {}
    try:
        for step in scenario["steps"]:
            if "call" in step:
                call = step["call"]
                rec = start_call(call, commanders)
                res = await await_cap(rec, call_ms)
                check_expect(name, call["expect"], res, rec)
            elif "callAsync" in step:
                call = step["callAsync"]
                named[call["name"]] = start_call(call, commanders)
            elif "awaitProgress" in step:
                await barrier(name, require_call(named, step["awaitProgress"]), barrier_ms)
            elif "cancel" in step:
                require_call(named, step["cancel"])["signal"].abort()
            elif "barrier" in step:
                # Driver rendezvous: emit a flushed PARLEY-BARRIER line and continue. The driver
                # watches the log stream for this and performs the container action.
                b = step["barrier"]
                label = b.get("label") if isinstance(b, dict) else None
                print(f"PARLEY-BARRIER {label or name}", flush=True)
            elif "await" in step:
                ref = step["await"]
                rec = require_call(named, ref)
                res = await await_cap(rec, call_ms)
                check_expect(name, ref["expect"], res, rec)
            else:
                raise PeerAssert(f"{name}: unrecognized step {json.dumps(step)}")
        return {"scenario": name, "pass": True, "detail": ""}
    except PeerAssert as exc:
        return {"scenario": name, "pass": False, "detail": str(exc)}
    except Exception as exc:  # noqa: BLE001 - report any unexpected failure as a scenario failure
        return {"scenario": name, "pass": False, "detail": f"unexpected: {type(exc).__name__}: {exc}"}


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------
def deep_equal(a: object, b: object) -> bool:
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if a is None or b is None:
        return a is None and b is None
    if isinstance(a, dict) or isinstance(b, dict):
        if not (isinstance(a, dict) and isinstance(b, dict)) or len(a) != len(b):
            return False
        return all(k in b and deep_equal(v, b[k]) for k, v in a.items())
    if isinstance(a, list) or isinstance(b, list):
        if not (isinstance(a, list) and isinstance(b, list)) or len(a) != len(b):
            return False
        return all(deep_equal(x, y) for x, y in zip(a, b))
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    return a == b


def get_field(request: object, key: str, default: object = None) -> object:
    return request[key] if isinstance(request, dict) and key in request else default


def num_field(request: object, key: str, default: int) -> int:
    v = get_field(request, key)
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else default


def topics(prefix: str, cmd: str) -> ParleyTopics:
    return ParleyTopics(
        request=f"{prefix}/{cmd}/req",
        heartbeat=f"{prefix}/{cmd}/hb",
        progress=f"{prefix}/{cmd}/prg",
        result=f"{prefix}/{cmd}/res",
    )


def build_config(c: dict, max_active: int) -> ParleyConfig:
    return ParleyConfig(
        request_retry_interval=c["requestRetryInterval"] / 1000,
        no_ack_timeout=c["noAckTimeout"] / 1000,
        stale_timeout=c["staleTimeout"] / 1000,
        execution_timeout=c["executionTimeout"] / 1000,
        cancel_retry_interval=c["cancelRetryInterval"] / 1000,
        heartbeat_interval=c["heartbeatInterval"] / 1000,
        result_retry_interval=c["resultRetryInterval"] / 1000,
        result_retry_timeout=c["resultRetryTimeout"] / 1000,
        tombstone_ttl=c["tombstoneTtl"] / 1000,
        max_concurrent_executions=c["maxConcurrentExecutions"],
        max_active_commands=max_active,
    )


def parse_args(argv: list[str]) -> dict[str, str]:
    out: dict[str, str] = {}
    i = 0
    while i < len(argv):
        if argv[i].startswith("--") and i + 1 < len(argv):
            out[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            i += 1
    return out


def parse_broker(broker: str) -> tuple[str, int]:
    s = broker
    scheme = s.find("://")
    if scheme >= 0:
        s = s[scheme + 3:]
    if ":" not in s:
        return s, 1883
    host, _, port = s.rpartition(":")
    return host, int(port)


async def wait_connected(client: mqtt.Client, timeout: float = 10.0) -> None:
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while not client.is_connected() and loop.time() < deadline:
        await asyncio.sleep(0.02)
    if not client.is_connected():
        raise RuntimeError("mqtt connection did not establish within timeout")


async def main() -> None:
    opt = parse_args(sys.argv[1:])
    role = opt["role"]
    host, port = parse_broker(opt["broker"])
    prefix = opt["prefix"]
    scenarios_path = opt.get("scenarios", "scenarios.json")
    suite_key = "selfChecks" if opt.get("suite") == "selfchecks" else "scenarios"
    handler_lang = opt.get("handler-lang", "-")
    scenario_filter = opt.get("scenario")
    qos = 1 if opt.get("qos") == "1" else 0

    with open(scenarios_path, encoding="utf-8") as f:
        root = json.load(f)
    cfg = root["config"]
    base_max_active = cfg.get("maxActiveCommands", 8)
    barrier_ms = root["timeouts"]["awaitProgressMs"]
    call_ms = root["timeouts"]["callMs"]

    loop = asyncio.get_running_loop()
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    # paho auto-reconnects on unexpected disconnect while loop_start's thread runs, refiring on_connect
    # (→ raise_connected → the core resubscribes). Cap the backoff at 2 s so a broker restart is bridged
    # well inside the deep staleTimeout (8 s).
    client.reconnect_delay_set(min_delay=1, max_delay=2)
    adapter = PahoAdapter(client, loop, qos)
    client.connect(host, port)
    client.loop_start()
    await wait_connected(client)

    if role == "handler":
        handlers = []
        for cmd in COMMANDS:
            max_active = 1 if cmd == "busy1" else base_max_active
            handlers.append(HarnessHandler(adapter, topics(prefix, cmd), build_config(cfg, max_active), cmd))
        await adapter.wait_for_subscriptions()
        print(READY_LINE, flush=True)
        await asyncio.Event().wait()  # serve until killed
        return

    commanders = {
        cmd: ParleyCommander(adapter, topics(prefix, cmd), build_config(cfg, base_max_active))
        for cmd in COMMANDS
    }
    await adapter.wait_for_subscriptions()

    results = []
    for scenario in root[suite_key]:
        if scenario_filter and scenario["name"] != scenario_filter:
            continue
        results.append(await run_scenario(scenario, commanders, barrier_ms, call_ms))
    if scenario_filter and not results:
        results.append({"scenario": scenario_filter, "pass": False,
                        "detail": f"no scenario named '{scenario_filter}' in suite '{suite_key}'"})

    all_pass = all(r["pass"] for r in results)
    report = json.dumps(
        {"commander": SELF_LANG, "handler": handler_lang, "prefix": prefix, "suite": suite_key,
         "results": results, "pass": all_pass},
        indent=2,
        ensure_ascii=False,
    )
    print(report)
    if "report" in opt:
        with open(opt["report"], "w", encoding="utf-8") as f:
            f.write(report)

    for c in commanders.values():
        await c.close()
    client.loop_stop()
    client.disconnect()
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    asyncio.run(main())
