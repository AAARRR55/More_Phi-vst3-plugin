"""Tests for the server's tool dispatch with input validation (fixes #1, #2, #5).

The dispatch logic is extracted out of the stdio ``call_tool`` closure into a
pure async function so it can be exercised without spawning the server
subprocess. These tests prove:

  * bad input is rejected BEFORE the handler runs (no more generic
    "Internal error" masking a client type/range error);
  * the failure carries the structured contract (code/parameter/received/
    valid_range);
  * good input still reaches the handler and returns success;
  * unknown tools still get the UnknownTool branch, not a validation error.
"""

from __future__ import annotations

import pytest

from bridge import VST3IPCBridge
from bridge.packets import CommandPacket, ResultPacket, ResultPacketHeader, ResultStatus
from server import ServerContext, dispatch_tool
from tools import ParameterRegistry


class _RecordingBridge(VST3IPCBridge):
    """Counts how many times a handler actually reached the bridge. If
    validation rejected the call, commands stays empty."""

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


def _ctx() -> ServerContext:
    # Build a minimal context. cache/audit are cheap; only the bridge matters.
    from cache import ToolResultCache
    from audit import AuditLogger
    return ServerContext(
        bridge=_RecordingBridge(),
        registry=ParameterRegistry(instance_id="test"),
        cache=ToolResultCache(),
        audit=AuditLogger(),
    )


@pytest.mark.asyncio
async def test_dispatch_rejects_wrong_type_before_handler() -> None:
    ctx = _ctx()
    out = await dispatch_tool(ctx, "set_output_gain", {"gain_db": "loud"})
    assert out["status"] == "failure"
    assert out["code"] == "TYPE_ERROR"
    assert out["parameter"] == "gain_db"
    assert out["received"] == "loud"
    # The handler never ran -- the bridge recorded no commands.
    assert ctx.bridge.commands == []


@pytest.mark.asyncio
async def test_dispatch_rejects_out_of_range_before_handler() -> None:
    ctx = _ctx()
    out = await dispatch_tool(ctx, "set_compressor", {
        "ratio": 3.0, "threshold_db": -80.0, "attack_ms": 10.0, "release_ms": 100.0,
    })
    assert out["status"] == "failure"
    assert out["code"] == "PARAM_OUT_OF_RANGE"
    assert out["parameter"] == "threshold_db"
    assert out["received"] == -80.0
    assert out["valid_range"] == [-60.0, 0.0]
    assert ctx.bridge.commands == []


@pytest.mark.asyncio
async def test_dispatch_rejects_missing_required_before_handler() -> None:
    ctx = _ctx()
    out = await dispatch_tool(ctx, "set_output_gain", {})
    assert out["status"] == "failure"
    assert out["code"] == "MISSING_REQUIRED_PROPERTY"
    assert out["parameter"] == "gain_db"
    assert ctx.bridge.commands == []


@pytest.mark.asyncio
async def test_dispatch_passes_valid_input_to_handler() -> None:
    ctx = _ctx()
    out = await dispatch_tool(ctx, "set_output_gain", {"gain_db": -3.0})
    assert out["status"] == "success"
    # The handler ran -- the bridge recorded the SET_PARAM command.
    assert len(ctx.bridge.commands) == 1
    # dispatch_tool returns the raw handler output; latency_ms stamping is the
    # caller's (_finalize's) responsibility, so it must NOT be present here.
    assert "latency_ms" not in out


@pytest.mark.asyncio
async def test_dispatch_unknown_tool_returns_unknown_branch() -> None:
    ctx = _ctx()
    out = await dispatch_tool(ctx, "no_such_tool", {"x": 1})
    assert out["status"] == "failure"
    # UnknownTool branch keeps the legacy shape (no code field) -- that's the
    # caller's responsibility, validation did not fire.
    assert "Unknown tool" in out["error_message"]
