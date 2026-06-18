"""
Async IPC bridge client for the More-Phi VST3 host.

Connects to the C++ VST3IPCBridge over a named pipe (Windows) or Unix domain
socket (macOS/Linux), sends CommandPackets, and resolves ResultPackets by
command_id.
"""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field
from typing import Any

from .packets import (
    RESULT_HEADER_SIZE,
    CommandPacket,
    CommandType,
    ResultPacket,
    ResultPacketHeader,
    ResultStatus,
)
from .transport import Transport, connect_transport, default_endpoint


class VST3IPCError(Exception):
    """Raised when the IPC bridge fails or times out."""

    def __init__(self, message: str, code: int = -32001) -> None:
        super().__init__(message)
        self.code = code


@dataclass
class VST3IPCBridge:
    endpoint: str = field(default_factory=default_endpoint)
    max_concurrent: int = 8
    default_timeout: float = 2.0

    transport: Transport | None = field(default=None, repr=False)
    pending: dict[int, asyncio.Future[ResultPacket]] = field(
        default_factory=dict, repr=False
    )
    seq: int = field(default=1, repr=False)
    seq_lock: asyncio.Lock = field(default_factory=asyncio.Lock, repr=False)
    semaphore: asyncio.Semaphore = field(init=False, repr=False)
    reader_task: asyncio.Task[None] | None = field(default=None, repr=False)
    connected: bool = field(default=False, repr=False)

    def __post_init__(self) -> None:
        self.semaphore = asyncio.Semaphore(self.max_concurrent)

    async def connect(self) -> None:
        """Connect to the C++ host and start the result reader."""
        if self.connected:
            return
        if self.transport is None:
            self.transport = await connect_transport(self.endpoint)
        self.reader_task = asyncio.create_task(self._result_reader_loop())
        self.connected = True

    async def disconnect(self) -> None:
        """Close the connection and cancel pending futures."""
        self.connected = False
        if self.reader_task is not None:
            self.reader_task.cancel()
            try:
                await self.reader_task
            except asyncio.CancelledError:
                pass
            self.reader_task = None
        if self.transport is not None:
            await self.transport.close()
            if not hasattr(self.transport, "reconnect_count"):
                self.transport = None

        for fut in self.pending.values():
            if not fut.done():
                fut.set_exception(VST3IPCError("Bridge disconnected", code=-32001))
        self.pending.clear()

    async def reconnect(self) -> None:
        """Disconnect and reconnect."""
        await self.disconnect()
        await self.connect()

    async def send_command(
        self,
        cmd: CommandPacket,
        timeout: float | None = None,
    ) -> ResultPacket:
        """Send a command and await its matched result."""
        if timeout is None:
            timeout = self.default_timeout

        await self.connect()
        assert self.transport is not None

        async with self.seq_lock:
            cmd.header.command_id = self.seq
            self.seq += 1
            command_id = cmd.header.command_id

        loop = asyncio.get_running_loop()
        fut: asyncio.Future[ResultPacket] = loop.create_future()
        self.pending[command_id] = fut

        try:
            async with self.semaphore:
                data = cmd.serialize()
                await self.transport.write(data)
        except Exception as e:
            self.pending.pop(command_id, None)
            if not fut.done():
                fut.set_exception(e)

        try:
            return await asyncio.wait_for(fut, timeout=timeout)
        except asyncio.TimeoutError as e:
            self.pending.pop(command_id, None)
            raise VST3IPCError(
                f"VST3 command {command_id} timed out", code=-32001
            ) from e

    async def _result_reader_loop(self) -> None:
        """Background coroutine: read result headers + payloads, resolve futures."""
        assert self.transport is not None
        try:
            while self.connected:
                header_data = await self.transport.read_exact(RESULT_HEADER_SIZE)
                header = ResultPacketHeader.deserialize(header_data)
                payload = b""
                if header.payload_length > 0:
                    payload = await self.transport.read_exact(header.payload_length)

                result = ResultPacket(header=header, payload=payload)
                fut = self.pending.pop(header.command_id, None)
                if fut is not None and not fut.done():
                    fut.set_result(result)
        except asyncio.CancelledError:
            raise
        except Exception as e:
            self.connected = False
            for fut in self.pending.values():
                if not fut.done():
                    fut.set_exception(e)
            self.pending.clear()

    # ------------------------------------------------------------------
    # Convenience helpers used by tool handlers.
    # ------------------------------------------------------------------
    async def set_parameter(
        self, param_id: int, normalized_value: float, timeout: float | None = None
    ) -> ResultPacket:
        return await self.send_command(
            CommandPacket.set_param(0, param_id, normalized_value), timeout=timeout
        )

    async def get_state(self, timeout: float | None = None) -> ResultPacket:
        return await self.send_command(CommandPacket.get_state(0), timeout=timeout)

    async def load_preset(
        self, preset_name: str, timeout: float | None = None
    ) -> ResultPacket:
        return await self.send_command(
            CommandPacket.load_preset(0, preset_name), timeout=timeout
        )

    async def batch(
        self, params: list[tuple[int, float]], timeout: float | None = None
    ) -> ResultPacket:
        return await self.send_command(
            CommandPacket.batch(0, params), timeout=timeout
        )
