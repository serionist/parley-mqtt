"""Unit tests for the ``json_equal`` deep-equality helper (TQ2 — bool/int must stay distinct)."""

from __future__ import annotations

from support.json_equal import json_equal


def test_bool_is_not_equal_to_int() -> None:
    # The whole point of the port: Python's == says {"ok": True} == {"ok": 1}; json_equal must not.
    assert not json_equal({"ok": True}, {"ok": 1})
    assert not json_equal({"ok": False}, {"ok": 0})
    assert not json_equal(True, 1)
    assert not json_equal(0, False)


def test_matching_bools_and_ints_are_equal() -> None:
    assert json_equal({"ok": True}, {"ok": True})
    assert json_equal({"n": 1}, {"n": 1})
    assert json_equal(False, False)


def test_number_exactness() -> None:
    assert json_equal(3.14, 3.14)
    assert not json_equal(3.14, 3.15)
    # int/float value equality is preserved (JSON 3 and 3.0 denote the same number, as in the TS port).
    assert json_equal(3, 3.0)
    assert not json_equal(3, 4)


def test_nested_structures() -> None:
    assert json_equal(
        {"a": [1, {"b": True}], "c": None},
        {"c": None, "a": [1, {"b": True}]},  # key order is irrelevant
    )
    # A bool buried inside a list still breaks equality against an int.
    assert not json_equal({"a": [1, True]}, {"a": [1, 1]})
    # Nested value difference.
    assert not json_equal({"a": [1, {"b": True}]}, {"a": [1, {"b": False}]})


def test_type_mismatches() -> None:
    assert not json_equal([1, 2], {"0": 1, "1": 2})
    assert not json_equal({"a": 1}, [1])
    assert not json_equal([1, 2, 3], [1, 2])
    assert not json_equal(None, 0)
    assert not json_equal("1", 1)


def test_string_and_null_equality() -> None:
    assert json_equal("x", "x")
    assert not json_equal("x", "y")
    assert json_equal(None, None)
