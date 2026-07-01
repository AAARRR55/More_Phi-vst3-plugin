"""Tests for the input-validation pipeline (audit fixes #1, #2, #3, #5).

The MCP server must validate tool arguments against the declared inputSchema
BEFORE dispatching to a handler, rejecting (not silently clamping) out-of-range
values, and emitting structured errors the LLM can branch on.
"""

from __future__ import annotations

import pytest

from tools.validation import ValidationError, validate_arguments


# ---------------------------------------------------------------------------
# Acceptance: valid input passes through untouched
# ---------------------------------------------------------------------------

def test_validate_accepts_valid_input() -> None:
    # Returns None (no raise) for well-formed, in-range input.
    assert validate_arguments("set_output_gain", {"gain_db": -3.0}) is None
    assert validate_arguments("set_eq_band", {
        "band_index": 0, "frequency_hz": 1000.0, "gain_db": 2.5, "q_factor": 1.4,
    }) is None
    assert validate_arguments("set_saturation", {"drive": 50.0, "mode": "tape"}) is None


def test_validate_accepts_no_arg_tools() -> None:
    assert validate_arguments("get_plugin_state", {}) is None
    assert validate_arguments("list_parameters", {}) is None


def test_validate_unknown_tool_passes_through() -> None:
    # Unknown tools are handled by the caller (server returns UnknownTool); the
    # validator must not raise for a name it has no schema for.
    assert validate_arguments("does_not_exist", {"anything": True}) is None


# ---------------------------------------------------------------------------
# Fix #1: structural validation (types, required fields)
# ---------------------------------------------------------------------------

def test_validate_rejects_missing_required_field() -> None:
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("set_output_gain", {})
    err = exc_info.value
    assert err.code == "MISSING_REQUIRED_PROPERTY"
    assert err.parameter == "gain_db"
    assert "gain_db" in err.args[0]


def test_validate_rejects_wrong_type() -> None:
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("set_output_gain", {"gain_db": "loud"})
    err = exc_info.value
    assert err.code == "TYPE_ERROR"
    assert err.parameter == "gain_db"
    assert err.received == "loud"


def test_validate_rejects_string_band_index() -> None:
    # The exact path that used to surface as generic "Internal error:
    # invalid literal for int()". Must now be a structured TYPE_ERROR.
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("set_eq_band", {
            "band_index": "foo", "frequency_hz": 1000.0, "gain_db": 1.0, "q_factor": 1.0,
        })
    err = exc_info.value
    assert err.code == "TYPE_ERROR"
    assert err.parameter == "band_index"


# ---------------------------------------------------------------------------
# Fix #2: range validation (reject, don't clamp, out-of-range values)
# ---------------------------------------------------------------------------

def test_validate_rejects_below_minimum_with_bounds() -> None:
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("set_compressor", {
            "ratio": 3.0, "threshold_db": -80.0, "attack_ms": 10.0, "release_ms": 100.0,
        })
    err = exc_info.value
    assert err.code == "PARAM_OUT_OF_RANGE"
    assert err.parameter == "threshold_db"
    assert err.received == -80.0
    assert err.valid_range == [-60.0, 0.0]


def test_validate_rejects_above_maximum_with_bounds() -> None:
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("set_eq_band", {
            "band_index": 0, "frequency_hz": 30000.0, "gain_db": 1.0, "q_factor": 1.0,
        })
    err = exc_info.value
    assert err.code == "PARAM_OUT_OF_RANGE"
    assert err.parameter == "frequency_hz"
    assert err.received == 30000.0
    assert err.valid_range == [20.0, 20000.0]


def test_validate_rejects_bad_enum() -> None:
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("set_saturation", {"drive": 50.0, "mode": "fuzz"})
    err = exc_info.value
    assert err.code == "ENUM_VIOLATION"
    assert err.parameter == "mode"
    assert err.received == "fuzz"
    assert err.valid_range == ["tape", "tube", "transistor"]


def test_validate_rejects_out_of_range_in_nested_array() -> None:
    # apply_mastering_chain -> eq_bands[].gain_db out of range must still resolve
    # the offending parameter path.
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("apply_mastering_chain", {
            "eq_bands": [{"band_index": 0, "frequency_hz": 1000.0, "gain_db": 99.0, "q_factor": 1.0}],
        })
    err = exc_info.value
    assert err.code == "PARAM_OUT_OF_RANGE"
    assert err.parameter == "gain_db"
    assert err.received == 99.0


# ---------------------------------------------------------------------------
# Fix #3: structured error contract
# ---------------------------------------------------------------------------

def test_validation_error_to_dict_has_full_contract() -> None:
    err = ValidationError(
        "threshold_db value -80.0 is below minimum -60.0.",
        code="PARAM_OUT_OF_RANGE",
        parameter="threshold_db",
        received=-80.0,
        valid_range=[-60.0, 0.0],
        suggestion="Use a value between -60.0 and 0.0 dB.",
    )
    d = err.to_error_dict()
    # The structured fields the audit requires.
    assert d["code"] == "PARAM_OUT_OF_RANGE"
    assert d["parameter"] == "threshold_db"
    assert d["received"] == -80.0
    assert d["valid_range"] == [-60.0, 0.0]
    assert d["suggestion"] == "Use a value between -60.0 and 0.0 dB."
    assert d["error"] is True
    # Backward-compatible fields the existing contract relies on.
    assert d["status"] == "failure"
    assert "error_message" in d
    assert "corrective_action" in d
