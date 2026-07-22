"""Command-id generator (protocol.md §4.2).

Injectable per the design so constrained devices can supply a cheaper source. Ids MUST be 1–64 chars
from ``[A-Za-z0-9_-]`` and carry at least 64 bits of randomness.
"""

from __future__ import annotations

import secrets
from typing import Protocol, runtime_checkable


@runtime_checkable
class ParleyIdGenerator(Protocol):
    """Returns fresh, valid command ids (protocol.md §4.2)."""

    def new_id(self) -> str:
        """Return a fresh, valid command id."""
        ...


class DefaultIdGenerator:
    """The default generator: 128 bits of randomness rendered as URL-safe Base64 (no padding).

    Every output character is within ``[A-Za-z0-9_-]`` and the id is 22 characters long, well within
    the §4.2 1–64 bound.
    """

    __slots__ = ()

    def new_id(self) -> str:
        return secrets.token_urlsafe(16)
