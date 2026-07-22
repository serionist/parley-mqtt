"""Envelope codec (protocol.md §4) plus the §4/§4.2 robustness checks.

The codec knows nothing about typed payloads; the payload ``p`` crosses the boundary as a raw
JSON value (a Python ``dict``/``list``/``str``/``int``/``float``/``bool``/``None``).
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Final

#: The single protocol version this document defines (protocol.md §4).
PROTOCOL_VERSION: Final = 1


class MessageType:
    """Wire message-type strings (protocol.md §4)."""

    REQUEST: Final = "req"
    HEARTBEAT: Final = "hb"
    PROGRESS: Final = "prg"
    RESULT: Final = "res"
    RESULT_ACK: Final = "rak"
    CANCEL: Final = "cnl"


#: Sentinel: a field that is absent from the wire object (distinct from a present JSON ``null``).
class _Absent:
    __slots__ = ()

    def __repr__(self) -> str:  # pragma: no cover - debug aid
        return "ABSENT"


ABSENT: Final = _Absent()

_ID_RE: Final = re.compile(r"[A-Za-z0-9_-]{1,64}")


@dataclass(frozen=True, slots=True)
class InboundEnvelope:
    """A parsed inbound envelope. ``p`` is the raw JSON payload value.

    ``p`` is ``None`` when the ``p`` field is absent *or* present as JSON ``null`` (protocol.md §4:
    "omitted/null means no payload"). ``ok``/``e``/``m`` are ``None`` when absent or of the wrong
    type (the per-type conditional requirements are checked by the caller).
    """

    v: int
    t: str
    id: str
    p: object | None
    ok: bool | None
    e: str | None
    m: str | None


def is_valid_id(command_id: str) -> bool:
    """Validate a command id against protocol.md §4.2 (1–64 chars from ``[A-Za-z0-9_-]``)."""
    return _ID_RE.fullmatch(command_id) is not None


def to_payload(value: object | None) -> object:
    """Normalize a typed payload for the ``p`` field.

    A ``None`` value maps to :data:`ABSENT` (protocol.md §4: "omitted/null means no payload"); any
    other value — including ``0``, ``False``, ``""``, ``[]``, ``{}`` — passes through as raw JSON.
    """
    return ABSENT if value is None else value


def serialize(
    t: str,
    command_id: str,
    *,
    p: object = ABSENT,
    ok: object = ABSENT,
    e: object = ABSENT,
    m: object = ABSENT,
) -> bytes:
    """Serialize an outbound envelope to UTF-8 JSON bytes. :data:`ABSENT` fields are omitted."""
    out: dict[str, object] = {"v": PROTOCOL_VERSION, "t": t, "id": command_id}
    if p is not ABSENT:
        out["p"] = p
    if ok is not ABSENT:
        out["ok"] = ok
    if e is not ABSENT:
        out["e"] = e
    if m is not ABSENT:
        out["m"] = m
    return json.dumps(out, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def parse(payload: bytes) -> InboundEnvelope | None:
    """Parse payload bytes into an :class:`InboundEnvelope`, applying §4 envelope-level robustness.

    Returns ``None`` for bytes that are not UTF-8, not a JSON object, or that lack a required field
    (``v`` integer, ``t`` string, ``id`` string). Unknown extra fields are ignored (forward
    compatibility). Per-type conditional requirements (``ok`` on ``res``; ``e`` when ``ok:false``)
    and §4.2 id validity are checked by the caller.
    """
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        return None

    try:
        value = json.loads(text)
    except (ValueError, TypeError):
        return None

    # Must be a JSON object (not an array, scalar, or null).
    if not isinstance(value, dict):
        return None

    v = value.get("v")
    t = value.get("t")
    command_id = value.get("id")

    # ``v`` must be an integer. ``bool`` is a subclass of ``int`` in Python — exclude it so a JSON
    # ``true`` never masquerades as a version.
    if not isinstance(v, int) or isinstance(v, bool):
        return None
    if not isinstance(t, str):
        return None
    if not isinstance(command_id, str):
        return None

    # "omitted/null means no payload" (§4): a present-null payload is normalized to absent (None).
    raw_p = value.get("p")

    ok_raw = value.get("ok")
    e_raw = value.get("e")
    m_raw = value.get("m")

    return InboundEnvelope(
        v=v,
        t=t,
        id=command_id,
        p=raw_p,
        ok=ok_raw if isinstance(ok_raw, bool) else None,
        e=e_raw if isinstance(e_raw, str) else None,
        m=m_raw if isinstance(m_raw, str) else None,
    )
