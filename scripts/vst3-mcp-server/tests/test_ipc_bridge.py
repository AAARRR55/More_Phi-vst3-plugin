"""
Tests for bridge.ipc_bridge.VST3IPCBridge.

Uses an in-memory mock transport so the suite can run without a live C++ host.
"""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any

import pytest

from bridge import VST3IPCBridge, VST3IPCError
from bridge.packets import (
    CommandPacket,
    CommandPacketHeader,
    CommandType,
    RESULT_HEADER_SIZE,
    ResultPacket,
    ResultPacketHeader,
    ResultStatus,
)
from bridge.transport import Transport


@dataclass
class MockTransport(Transport):
    """Async in-memory transport for testing."""

    responses: asyncio.Queue[bytes] = field(default_factory=asyncio.Queue)
    written: list[bytes] = field(default_factory=list)
    _open: bool = True

    async def read_exact(self, n: int, timeout: float | None = None) -> bytes:
        data = b""
        while len(data) < n:
            chunk = await asyncio.wait_for(self.responses.get(), timeout=timeout)
            data += chunk
        return data[:n]

    async def write(self, data: bytes) -> None:
        self.written.append(data)

    async def close(self) -> None:
        self._open = False

    @property
    def is_open(self) -> bool:
        return self._open

    def queue_result(self, result: ResultPacket) -> None:
        data = result.serialize()
        # Feed the header and payload as separate chunks to exercise read_exact.
        self.responses.put_nowait(data[:RESULT_HEADER_SIZE])
        if len(data) > RESULT_HEADER_SIZE:
            self.responses.put_nowait(data[RESULT_HEADER_SIZE:])


@pytest.fixture
def transport() -> MockTransport:
    return MockTransport()


@pytest.fixture
async def bridge(transport: MockTransport) -> VST3IPCBridge:
    b = VST3IPCBridge(endpoint="mock")
    b.transport = transport
    b.connected = True
    b.reader_task = asyncio.create_task(b._result_reader_loop())
    yield b
    b.connected = False
    if b.reader_task is not None:
        b.reader_task.cancel()
        try:
            await b.reader_task
        except asyncio.CancelledError:
            pass


@pytest.mark.asyncio
async def test_send_command_returns_result(bridge: VST3IPCBridge, transport: MockTransport) -> None:
    transport.queue_result(
        ResultPacket(
            header=ResultPacketHeader(
                command_id=1,
                status=ResultStatus.SUCCESS,
                value_before=0.0,
                value_after=0.75,
                timestamp_ns=1_000_000,
            )
        )
    )

    cmd = CommandPacket(
        header=CommandPacketHeader(
            command_type=CommandType.SET_PARAM,
            param_id=3,
            normalized_value=0.75,
        )
    )
    result = await bridge.send_command(cmd)

    assert result.header.command_id == 1
    assert result.header.status == ResultStatus.SUCCESS
    assert result.header.value_after == pytest.approx(0.75)


@pytest.mark.asyncio
async def test_command_id_is_monotonic(bridge: VST3IPCBridge, transport: MockTransport) -> None:
    ids: list[int] = []
    for i in range(1, 4):
        transport.queue_result(
            ResultPacket(
                header=ResultPacketHeader(
                    command_id=i,
                    status=ResultStatus.SUCCESS,
                    value_before=0.0,
                    value_after=float(i) * 0.1,
                    timestamp_ns=i * 1_000_000,
                )
            )
        )
        result = await bridge.send_command(
            CommandPacket(
                header=CommandPacketHeader(command_type=CommandType.SET_PARAM)
            )
        )
        ids.append(result.header.command_id)

    assert ids == [1, 2, 3]


@pytest.mark.asyncio
async def test_timeout_raises(bridge: VST3IPCBridge) -> None:
    with pytest.raises(VST3IPCError):
        await bridge.send_command(
            CommandPacket(header=CommandPacketHeader(command_type=CommandType.GET_STATE)),
            timeout=0.05,
        )


@pytest.mark.asyncio
async def test_semaphore_limits_concurrency(bridge: VST3IPCBridge) -> None:
    assert bridge.semaphore._value == 8


@pytest.mark.asyncio
async def test_unsolicited_result_is_ignored(bridge: VST3IPCBridge, transport: MockTransport) -> None:
    orphan = ResultPacket(
        header=ResultPacketHeader(
            command_id=9999,
            status=ResultStatus.SUCCESS,
        )
    )
    transport.queue_result(orphan)
    await asyncio.sleep(0.05)

    transport.queue_result(
        ResultPacket(
            header=ResultPacketHeader(
                command_id=1,
                status=ResultStatus.SUCCESS,
                value_after=0.8,
            )
        )
    )
    result = await bridge.send_command(
        CommandPacket(header=CommandPacketHeader(command_type=CommandType.SET_PARAM))
    )
    assert result.header.value_after == pytest.approx(0.8)
