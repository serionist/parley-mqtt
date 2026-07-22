"""Parley example (Python / asyncio) — a ``backup`` command over MQTT.

Two roles, one command type:

  * the HANDLER runs where the disk lives. It receives a backup request, streams
    progress while it works, and returns how many bytes it wrote — or a typed
    error (the disk filled up, the target was unreachable).

  * the COMMANDER runs wherever you start a backup. It calls the command,
    watches progress, can cancel, and gets one typed answer back.

Both talk over the MQTT broker you already run. Parley never opens the
connection itself — you hand it a live one through a small adapter (below).

The command in numbers::

    request   {"target": str}          — what to back up
    progress  {"percent": number}      — advisory, streamed while working
    result    {"bytesWritten": int}    — the single terminal success value
    errors    "diskFull" | "targetUnreachable"  — your own typed error codes

Run it as::

    python backup_example.py handler
    python backup_example.py commander
"""

from __future__ import annotations

import asyncio
import sys
from typing import Literal, TypedDict

import paho.mqtt.client as mqtt

from parley import (
    ParleyBuiltinError,
    ParleyCancelSignal,
    ParleyCommander,
    ParleyCommandHandler,
    ParleyConnectionAdapter,
    ParleyExecution,
    ParleyMessageHandler,
    ParleyOutcome,
    ParleyTopics,
)

# ---------------------------------------------------------------------------
# 1. The message shapes for this command.
# ---------------------------------------------------------------------------
# Plain TypedDicts: on the wire they are ordinary JSON objects, and Parley binds
# them with the four type parameters <Request, Progress, Result, Error>. No
# schema compiler, no code generation — your type hints are the contract.


class BackupRequest(TypedDict):
    target: str


class BackupProgress(TypedDict):
    percent: float


class BackupResult(TypedDict):
    bytesWritten: int


# The custom error codes this command can fail with — plain strings on the wire,
# type-checked here. (A custom code must not start with "$"; that is reserved for
# Parley's builtins.)
BackupError = Literal["diskFull", "targetUnreachable"]

# The four topic strings. Parley imposes no naming scheme — these are yours, and
# your broker's ACLs apply to them directly. Both roles must agree on the set.
BACKUP_TOPICS = ParleyTopics(
    request="home/backup/req",  # commander → handler: req, rak, cnl
    heartbeat="home/backup/hb",  # handler → commander: heartbeats
    progress="home/backup/prg",  # handler → commander: progress
    result="home/backup/res",  # handler → commander: the result
)


# ===========================================================================
# 2. An ILLUSTRATIVE transport adapter over paho-mqtt.
# ===========================================================================
# ⚠️  This adapter is hand-written for the example. It has NOT been shipped as a
#     package or run through CI — it shows the shape you would implement over
#     your own client. A verified, reusable adapter arrives with the e2e suite.
#
# One threading detail matters (and is easy to get wrong): paho runs its network
# loop on a BACKGROUND thread (`loop_start()`), but the Parley core is asyncio
# and must only be touched from the event-loop thread. So every inbound paho
# callback is marshalled back onto the loop with `call_soon_threadsafe`.
class PahoAdapter(ParleyConnectionAdapter):
    def __init__(self, client: mqtt.Client, loop: asyncio.AbstractEventLoop) -> None:
        super().__init__()  # required — the base sets up the connected-callback list
        self._client = client
        self._loop = loop
        self._handlers: dict[str, ParleyMessageHandler] = {}
        client.on_message = self._on_message
        client.on_connect = self._on_connect

    # Fire-and-forget publish at QoS 0, retain=False (all Parley requires).
    def publish(self, topic: str, payload: bytes) -> None:
        self._client.publish(topic, payload, qos=0, retain=False)

    # Replace-semantics: one handler per topic. Record it, and if we are already
    # connected, subscribe at the broker now.
    def subscribe(self, topic: str, on_message: ParleyMessageHandler) -> None:
        self._handlers[topic] = on_message
        if self._client.is_connected():
            self._client.subscribe(topic, qos=0)

    def unsubscribe(self, topic: str) -> None:
        self._handlers.pop(topic, None)
        if self._client.is_connected():
            self._client.unsubscribe(topic)

    # --- paho callbacks (run on paho's network thread) → hop onto the loop ---
    def _on_connect(self, _client: object, _userdata: object, _flags: object,
                    _reason: object, _props: object = None) -> None:
        # Telling Parley we (re)connected makes it re-subscribe its topics, which
        # is how a dropped connection self-heals.
        self._loop.call_soon_threadsafe(self.raise_connected)

    def _on_message(self, _client: object, _userdata: object, message: mqtt.MQTTMessage) -> None:
        handler = self._handlers.get(message.topic)
        if handler is not None:
            self._loop.call_soon_threadsafe(handler, message.topic, message.payload)


# ===========================================================================
# 3. The HANDLER — the side that performs the backup.
# ===========================================================================
# Subclass ParleyCommandHandler and implement handle_command. Expected failures
# are RETURNED as ParleyOutcome values, never raised — raising is reserved for
# genuine bugs (which Parley contains: the command aborts, nothing leaks).
class BackupHandler(ParleyCommandHandler[BackupRequest, BackupProgress, BackupResult, BackupError]):
    async def handle_command(
        self, request: BackupRequest, execution: ParleyExecution[BackupProgress]
    ) -> ParleyOutcome[BackupResult, BackupError]:
        # A made-up backup that "copies" ten chunks. While it runs, Parley is
        # heartbeating this command every few seconds on your behalf, so the
        # caller can always tell "slow" from "dead" — you never fake progress
        # just to stay trusted.
        bytes_written = 0

        for chunk in range(1, 11):
            # Cooperative cancellation: a wire-level `cnl` from the caller flips
            # execution.signal.aborted. Check it and stop cleanly.
            if execution.signal.aborted:
                return ParleyOutcome.cancelled()  # → the caller sees $cancelled

            wrote = await copy_one_chunk(request["target"], chunk)
            if wrote < 0:
                # Domain failures are typed values — returned to the caller typed.
                return ParleyOutcome.error("diskFull", "no space left on device")
            bytes_written += wrote

            # Advisory progress. Each call publishes one `prg`; delivery is
            # best-effort (never acknowledged), so it is safe to send often.
            execution.report_progress({"percent": chunk * 10})

        # The single terminal success value.
        result: BackupResult = {"bytesWritten": bytes_written}
        return ParleyOutcome.success(result)


# ===========================================================================
# 4. The COMMANDER — the side that requests a backup.
# ===========================================================================
async def request_backup(
    commander: ParleyCommander[BackupRequest, BackupProgress, BackupResult, BackupError],
) -> None:
    # A cancel signal you can trip any time before the result lands; tripping it
    # sends a real `cnl` to the handler.
    signal = ParleyCancelSignal()

    # Auto-cancel this backup if it somehow runs longer than five minutes.
    loop = asyncio.get_running_loop()
    give_up = loop.call_later(5 * 60, signal.abort)

    request: BackupRequest = {"target": "photos"}
    result = await commander.execute(
        request,
        on_progress=lambda p: print(f"backup {p['percent']}% done"),
        signal=signal,
    )

    give_up.cancel()

    # Every outcome — success, your errors, and all six builtins — is a value.
    if result.is_success:
        assert result.value is not None
        print(f"backup complete: {result.value['bytesWritten']} bytes written")
        return

    error = result.error
    assert error is not None
    if error.is_builtin:
        # A builtin: something about delivery/liveness, not your domain.
        if error.builtin == ParleyBuiltinError.NO_ACK:
            print("no handler answered — is the device online?")
        elif error.builtin == ParleyBuiltinError.BUSY:
            print("the device is at capacity — retry shortly")
        elif error.builtin == ParleyBuiltinError.EXECUTION_STALE:
            print("the device went silent mid-backup")
        elif error.builtin == ParleyBuiltinError.EXECUTION_TIMEOUT:
            print("the backup ran past its time cap")
        elif error.builtin == ParleyBuiltinError.CANCELLED:
            print("the backup was cancelled")
        elif error.builtin == ParleyBuiltinError.DECODE_ERROR:
            print("the device sent a result we could not read")
    else:
        # A custom code — typed as BackupError.
        if error.custom == "diskFull":
            print(f"out of space: {error.message or ''}")
        elif error.custom == "targetUnreachable":
            print("the backup target could not be reached")


# ===========================================================================
# 5. Wiring it up.
# ===========================================================================
async def main() -> None:
    role = sys.argv[1] if len(sys.argv) > 1 else "commander"

    # YOU own the connection: host, credentials, and TLS all live here, never in
    # Parley. paho's loop runs on its own thread; we hand the adapter our loop so
    # it can marshal callbacks back safely.
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
    # client.username_pw_set(...) / client.tls_set(...) go here — Parley never sees them.
    client.connect("localhost", 1883)
    client.loop_start()

    loop = asyncio.get_running_loop()
    adapter = PahoAdapter(client, loop)

    if role == "handler":
        handler = BackupHandler(adapter, BACKUP_TOPICS)
        print("backup handler ready")
        try:
            await asyncio.Event().wait()  # serve until cancelled
        finally:
            await handler.close()  # cancels timers, signals handlers, frees state
            client.loop_stop()
    else:
        commander = ParleyCommander[BackupRequest, BackupProgress, BackupResult, BackupError](
            adapter, BACKUP_TOPICS
        )
        try:
            await request_backup(commander)
        finally:
            await commander.close()
            client.loop_stop()


async def copy_one_chunk(target: str, chunk: int) -> int:
    """A stand-in for real I/O. Returns bytes written, or -1 for a full disk."""
    await asyncio.sleep(0.2)
    return 1024 * chunk  # pretend each chunk is a little bigger


if __name__ == "__main__":
    asyncio.run(main())
