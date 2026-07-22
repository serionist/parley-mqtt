"""Deep JSON equality — a port of ``js/test/support/jsonEqual.ts`` (protocol.md §10 exact-set
matching): dicts compare by key set and values, lists element-wise in order, everything else by value.

Both operands are values produced by :func:`json.loads`, so ``None`` is the only null and no
``undefined`` ever appears. The one Python-specific wrinkle the port must guard is bool/int: Python's
``==`` conflates ``True == 1`` and ``False == 0``, which JSON semantics (and the TS ``jsonEqual``,
whose ``===`` sees ``typeof true === 'boolean' !== 'number'``) treat as distinct. The guard
``(type(a) is bool) != (type(b) is bool)`` — recursed through dicts and lists — restores that
distinction so a ``{"ok": true}`` expectation never matches a ``{"ok": 1}`` payload.
"""

from __future__ import annotations


def json_equal(a: object, b: object) -> bool:
    """Return whether two ``json.loads`` values are deeply equal, keeping bool distinct from int."""
    # Exactly one operand a bool => not equal (True is never 1, False is never 0), recursively.
    if (type(a) is bool) != (type(b) is bool):
        return False

    if isinstance(a, dict) or isinstance(b, dict):
        if not (isinstance(a, dict) and isinstance(b, dict)):
            return False
        if a.keys() != b.keys():
            return False
        return all(json_equal(a[key], b[key]) for key in a)

    if isinstance(a, list) or isinstance(b, list):
        if not (isinstance(a, list) and isinstance(b, list)):
            return False
        if len(a) != len(b):
            return False
        return all(json_equal(x, y) for x, y in zip(a, b, strict=True))

    return a == b
