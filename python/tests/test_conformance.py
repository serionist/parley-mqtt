"""Layer 2 — conformance vectors (protocol.md §10, docs/testing.md).

One test per ``docs/conformance/*.json`` via dynamic discovery, both roles, replayed over the fake
transport and virtual clock. The vectors are READ-ONLY and language-neutral; a disagreement is
resolved by re-reading the spec, never by editing a vector.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from support.vector_runner import run_vector

_CONFORMANCE_DIR = Path(__file__).resolve().parents[2] / "docs" / "conformance"
_VECTORS = sorted(_CONFORMANCE_DIR.glob("*.json"))


def test_vectors_discovered() -> None:
    # Guard against a silent glob miss that would make the suite vacuously pass.
    assert len(_VECTORS) == 41, f"expected 41 conformance vectors, found {len(_VECTORS)}"


@pytest.mark.parametrize("path", _VECTORS, ids=[p.stem for p in _VECTORS])
async def test_conformance_vector(path: Path) -> None:
    await run_vector(str(path))
