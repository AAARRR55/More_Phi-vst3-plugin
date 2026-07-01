"""Tests for apply_mastering_chain schema + handler completeness (audit fix #4).

The chain tool must:
  * reject empty input (was: returned success with 0 applied params);
  * require at least one top-level section (eq_bands/compressor/...);
  * apply compressor attack_ms AND release_ms (was: only ratio+threshold,
    silently dropping attack/release even though set_compressor requires them);
  * reject unknown properties (additionalProperties:false).
"""

from __future__ import annotations

import pytest

from bridge import VST3IPCBridge
from bridge.packets import CommandPacket, ResultPacket, ResultPacketHeader, ResultStatus
from tools import HANDLERS, ParameterRegistry, get_tool_descriptions
from tools.validation import ValidationError, validate_arguments


class _Bridge(VST3IPCBridge):
    def __init__(self) -> None:
        super().__init__(endpoint="fake")
        self.commands: list[CommandPacket] = []

    async def send_command(self, cmd: CommandPacket, timeout: float | None = None) -> ResultPacket:
        self.commands.append(cmd)
        return ResultPacket(
            header=ResultPacketHeader(
                command_id=cmd.header.command_id,
                status=ResultStatus.SUCCESS,
                value_before=0.5,
                value_after=0.6,
            )
        )

    async def connect(self) -> None:
        pass


def _schema() -> dict:
    return next(t for t in get_tool_descriptions() if t["name"] == "apply_mastering_chain")["inputSchema"]


# ---------------------------------------------------------------------------
# Schema: empty input must be rejected (was a no-op "success")
# ---------------------------------------------------------------------------

def test_empty_chain_input_is_rejected() -> None:
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("apply_mastering_chain", {})
    err = exc_info.value
    # Must reject -- the specific code matters less than that it IS rejected.
    assert err.code in ("MISSING_REQUIRED_PROPERTY", "SCHEMA_VIOLATION", "INSUFFICIENT_PROPERTIES")


def test_chain_requires_at_least_one_section() -> None:
    # The schema must express "at least one section required" -- either via a
    # top-level `required` or via `anyOf`/`oneOf` over the section properties.
    # We accept anyOf (the idiomatic JSON-Schema form for one-or-more-of).
    schema = _schema()
    assert schema.get("required") or schema.get("anyOf") or schema.get("oneOf"), (
        "apply_mastering_chain must require at least one section"
    )


# ---------------------------------------------------------------------------
# Schema: compressor object must carry attack/release (parity with set_compressor)
# ---------------------------------------------------------------------------

def test_chain_compressor_schema_has_attack_and_release() -> None:
    schema = _schema()
    comp_props = schema["properties"]["compressor"]["properties"]
    for p in ("ratio", "threshold_db", "attack_ms", "release_ms"):
        assert p in comp_props, f"chain.compressor missing {p}"


# ---------------------------------------------------------------------------
# Handler: attack_ms and release_ms are actually applied (not dropped)
# ---------------------------------------------------------------------------

@pytest.mark.asyncio
async def test_chain_applies_compressor_attack_and_release() -> None:
    bridge = _Bridge()
    registry = ParameterRegistry(instance_id="test")
    out = await HANDLERS["apply_mastering_chain"](
        bridge,
        {"compressor": {"ratio": 2.0, "threshold_db": -18.0, "attack_ms": 10.0, "release_ms": 100.0}},
        registry,
    )
    assert out["status"] in ("success", "partial_failure")
    # All four compressor params must be requested (applied + skipped covers them).
    assert out["requested_params"] >= 4
    assert out["applied_params"] >= 4  # all four resolve in the fallback registry


# ---------------------------------------------------------------------------
# Schema: additionalProperties:false to catch typos
# ---------------------------------------------------------------------------

def test_chain_rejects_unknown_top_level_property() -> None:
    with pytest.raises(ValidationError) as exc_info:
        validate_arguments("apply_mastering_chain", {"eq_bands": [], "eq_gainz": 1.0})
    assert exc_info.value.code in ("SCHEMA_VIOLATION", "ADDITIONAL_PROPERTIES")
