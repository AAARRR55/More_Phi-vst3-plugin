"""Tests for the tool registry and handler logic."""

from __future__ import annotations

import asyncio
from typing import Any

import pytest

from bridge import VST3IPCBridge
from bridge.normalizer import denormalize_gain_db, normalize_frequency_hz
from bridge.packets import CommandPacket, CommandType, ResultPacket, ResultPacketHeader, ResultStatus, parse_batch_payload
from tools import HANDLERS, ParameterRegistry, get_tool_descriptions


class FakeBridge(VST3IPCBridge):
    """Bridge that records commands and returns synthetic results."""

    def __init__(self) -> None:
        super().__init__(endpoint="fake")
        self.commands: list[CommandPacket] = []
        self._next_result_status = ResultStatus.SUCCESS

    async def send_command(self, cmd: CommandPacket, timeout: float | None = None) -> ResultPacket:
        self.commands.append(cmd)
        return ResultPacket(
            header=ResultPacketHeader(
                command_id=cmd.header.command_id,
                status=self._next_result_status,
                value_before=0.5,
                value_after=0.6,
            )
        )

    async def connect(self) -> None:
        pass


@pytest.fixture
def registry() -> ParameterRegistry:
    return ParameterRegistry(instance_id="test")


@pytest.fixture
def bridge() -> FakeBridge:
    return FakeBridge()


def test_tool_descriptions_are_valid() -> None:
    tools = get_tool_descriptions()
    assert len(tools) > 0
    required_keys = {"name", "description", "inputSchema", "outputSchema"}
    for tool in tools:
        assert required_keys.issubset(tool.keys())
        assert tool["inputSchema"]["type"] == "object"


def test_registry_loads_fallback(registry: ParameterRegistry) -> None:
    assert len(registry.parameters) > 0
    gain = registry.resolve(name="EQ_Band1_Gain")
    assert gain is not None
    assert gain.param_id == 1001


def test_handler_dispatch_exists() -> None:
    expected = {
        "set_eq_band",
        "set_output_gain",
        "set_compressor",
        "set_limiter_ceiling",
        "set_lufs_target",
        "apply_mastering_chain",
        "list_parameters",
    }
    assert expected.issubset(HANDLERS.keys())


@pytest.mark.asyncio
async def test_set_output_gain_handler(bridge: FakeBridge, registry: ParameterRegistry) -> None:
    result = await HANDLERS["set_output_gain"](bridge, {"gain_db": -3.0}, registry)
    assert result["status"] == "success"
    assert len(bridge.commands) == 1
    assert bridge.commands[0].header.command_type == CommandType.SET_PARAM
    assert bridge.commands[0].header.param_id == 5001


@pytest.mark.asyncio
async def test_set_eq_band_handler(bridge: FakeBridge, registry: ParameterRegistry) -> None:
    # The reported (gain) parameter is sent via SET_PARAM so its before/after can
    # be verified against the bridge result; freq+q ride in a batch. FakeBridge
    # reports value_before=0.5, value_after=0.6 for every command.
    result = await HANDLERS["set_eq_band"](
        bridge,
        {"band_index": 0, "frequency_hz": 1000, "gain_db": 2.5, "q_factor": 1.4},
        registry,
    )
    assert result["status"] == "success"
    types = [c.header.command_type for c in bridge.commands]
    assert CommandType.SET_PARAM in types
    assert CommandType.BATCH in types
    # The SET_PARAM targets the gain parameter (1001) and is verified.
    gain_set = [c for c in bridge.commands if c.header.command_type == CommandType.SET_PARAM]
    assert gain_set[0].header.param_id == 1001
    assert result["param_id"] == 1001
    # value_before/after are reported in PHYSICAL units (dB), denormalized from
    # the bridge's normalized readback (FakeBridge: 0.5 -> 0.6).
    assert result["value_before"] == pytest.approx(denormalize_gain_db(0.5))
    assert result["value_after"] == pytest.approx(denormalize_gain_db(0.6))


@pytest.mark.asyncio
async def test_apply_mastering_chain_reports_skipped_unresolved_params(
    bridge: FakeBridge, registry: ParameterRegistry
) -> None:
    # band_index 4 -> EQ_Band5_* which is absent from the fallback registry, so
    # those three params cannot be resolved and must be reported as skipped
    # (not silently dropped). output_gain (5001) still resolves and applies.
    result = await HANDLERS["apply_mastering_chain"](
        bridge,
        {
            "eq_bands": [
                {"band_index": 4, "frequency_hz": 1000, "gain_db": 1.0, "q_factor": 1.0}
            ],
            "output_gain_db": -3.0,
        },
        registry,
    )
    assert result["status"] == "partial_failure"
    assert result["applied_params"] >= 1
    assert result.get("skipped_params", 0) >= 1
    assert result.get("skipped")


@pytest.mark.asyncio
async def test_reset_to_default_sends_normalized_values(
    bridge: FakeBridge, registry: ParameterRegistry
) -> None:
    # reset_to_default must normalize each default against its parameter range
    # before sending -- the IPC batch expects normalized [0,1] values, NOT the
    # raw physical default (e.g. 1000.0 Hz). Sending 1000.0 would corrupt state.
    await HANDLERS["reset_to_default"](bridge, {}, registry)
    batch_cmds = [c for c in bridge.commands if c.header.command_type == CommandType.BATCH]
    assert batch_cmds, "reset_to_default should batch the parameter resets"
    pairs = parse_batch_payload(batch_cmds[0].payload)
    assert pairs, "batch payload should contain (param_id, value) pairs"
    for _param_id, value in pairs:
        assert 0.0 <= value <= 1.0, value
    # EQ_Band1_Freq (1002) default 1000 Hz must be normalized, not 1000.0.
    freq_value = next(v for pid, v in pairs if pid == 1002)
    assert freq_value == pytest.approx(normalize_frequency_hz(1000.0))


@pytest.mark.asyncio
async def test_apply_mastering_chain_handler(bridge: FakeBridge, registry: ParameterRegistry) -> None:
    result = await HANDLERS["apply_mastering_chain"](
        bridge,
        {
            "eq_bands": [
                {"band_index": 0, "frequency_hz": 1000, "gain_db": 1.0, "q_factor": 1.0}
            ],
            "compressor": {"ratio": 2.0, "threshold_db": -18.0, "attack_ms": 10.0, "release_ms": 100.0},
            "limiter_ceiling_db": -1.0,
            "output_gain_db": -3.0,
        },
        registry,
    )
    assert result["status"] == "success"
    assert result["applied_params"] > 0
