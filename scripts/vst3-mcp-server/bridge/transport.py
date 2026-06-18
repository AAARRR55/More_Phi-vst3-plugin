"""
Cross-platform transport for the VST3 IPC bridge.

Windows: named pipes via pywin32/win32file.
macOS/Linux: asyncio Unix domain sockets.
"""

from __future__ import annotations

import asyncio
import platform
import struct
import sys
import tempfile
from abc import ABC, abstractmethod
from pathlib import Path
from typing import Self


class Transport(ABC):
    """Abstract async transport for reading/writing raw bytes."""

    @abstractmethod
    async def read_exact(self, n: int, timeout: float | None = None) -> bytes:
        """Read exactly n bytes; raise asyncio.TimeoutError on timeout."""
        ...

    @abstractmethod
    async def write(self, data: bytes) -> None:
        ...

    @abstractmethod
    async def close(self) -> None:
        ...

    @property
    @abstractmethod
    def is_open(self) -> bool:
        ...


def _derive_instance_id(endpoint: str) -> str:
    """Extract the instance id suffix from an endpoint path/name."""
    pipe_prefix = "\\\\.\\pipe\\"
    if endpoint.startswith(pipe_prefix):
        name = endpoint[len(pipe_prefix) :]
        prefix = "more_phi_vst3_mcp_"
        if name.startswith(prefix):
            return name[len(prefix) :]
        return name
    path = Path(endpoint)
    stem = path.stem
    prefix = "more_phi_vst3_mcp_"
    if stem.startswith(prefix):
        return stem[len(prefix) :]
    return stem


def default_endpoint(instance_id: str | None = None) -> str:
    """Return the default endpoint for the current platform."""
    if instance_id is None:
        instance_id = "default"
    if platform.system() == "Windows":
        return "\\\\.\\pipe\\more_phi_vst3_mcp_" + instance_id
    return str(Path(tempfile.gettempdir()) / f"more_phi_vst3_mcp_{instance_id}.sock")


async def connect_transport(endpoint: str) -> Transport:
    """Connect to an endpoint, selecting the appropriate transport."""
    if platform.system() == "Windows":
        return await _WindowsPipeTransport.connect(endpoint)
    return await _UnixSocketTransport.connect(endpoint)


# ---------------------------------------------------------------------------
# Windows named-pipe implementation
# ---------------------------------------------------------------------------
class _WindowsPipeTransport(Transport):
    def __init__(self, handle: int) -> None:
        import win32file  # type: ignore

        self._handle = handle
        self._win32file = win32file

    @classmethod
    async def connect(cls, endpoint: str) -> Self:
        import pywintypes  # type: ignore
        import win32file

        # ConnectNamedPipe / CreateFile is synchronous; run in thread pool.
        loop = asyncio.get_running_loop()
        try:
            handle = await loop.run_in_executor(
                None,
                lambda: win32file.CreateFile(
                    endpoint,
                    win32file.GENERIC_READ | win32file.GENERIC_WRITE,
                    0,
                    None,
                    win32file.OPEN_EXISTING,
                    0,
                    None,
                ),
            )
        except pywintypes.error as e:
            raise ConnectionRefusedError(f"Could not connect to {endpoint}: {e}") from e

        # Switch to message-read mode is not required; we use byte stream mode.
        return cls(handle)

    async def read_exact(self, n: int, timeout: float | None = None) -> bytes:
        loop = asyncio.get_running_loop()
        chunks: list[bytes] = []
        remaining = n
        deadline = None
        if timeout is not None:
            deadline = loop.time() + timeout

        while remaining > 0:
            current_timeout = None
            if deadline is not None:
                current_timeout = max(0.0, deadline - loop.time())

            try:
                _, data = await asyncio.wait_for(
                    loop.run_in_executor(None, self._win32file.ReadFile, self._handle, remaining),
                    timeout=current_timeout,
                )
            except asyncio.TimeoutError:
                raise
            except Exception as e:
                raise ConnectionResetError(f"Named pipe read failed: {e}") from e

            if not data:
                raise ConnectionResetError("Named pipe closed")
            chunks.append(data)
            remaining -= len(data)

        return b"".join(chunks)

    async def write(self, data: bytes) -> None:
        loop = asyncio.get_running_loop()
        offset = 0
        while offset < len(data):
            try:
                _, written = await loop.run_in_executor(
                    None,
                    self._win32file.WriteFile,
                    self._handle,
                    data[offset:],
                )
            except Exception as e:
                raise ConnectionResetError(f"Named pipe write failed: {e}") from e
            if written == 0:
                raise ConnectionResetError("Named pipe write returned 0")
            offset += written

    async def close(self) -> None:
        try:
            self._win32file.CloseHandle(self._handle)
        except Exception:
            pass

    @property
    def is_open(self) -> bool:
        return True


# ---------------------------------------------------------------------------
# Unix domain socket implementation
# ---------------------------------------------------------------------------
class _UnixSocketTransport(Transport):
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self._reader = reader
        self._writer = writer

    @classmethod
    async def connect(cls, endpoint: str) -> Self:
        reader, writer = await asyncio.open_unix_connection(endpoint)
        return cls(reader, writer)

    async def read_exact(self, n: int, timeout: float | None = None) -> bytes:
        data = await asyncio.wait_for(self._reader.readexactly(n), timeout=timeout)
        return data

    async def write(self, data: bytes) -> None:
        self._writer.write(data)
        await self._writer.drain()

    async def close(self) -> None:
        if self._writer.is_closing():
            return
        self._writer.close()
        try:
            await self._writer.wait_closed()
        except Exception:
            pass

    @property
    def is_open(self) -> bool:
        return not self._writer.is_closing()


def make_transport_path(instance_id: str, platform: str) -> str:
    if platform == "win32":
        return "\\\\.\\pipe\\more_phi_vst3_mcp_" + instance_id
    return str(Path(tempfile.gettempdir()) / f"more_phi_vst3_mcp_{instance_id}.sock")


class MockTransport(Transport):
    def __init__(self) -> None:
        self.responses = bytearray()
        self.pending_replies: list[bytes] = []
        self.written: list[bytes] = []
        self.broken_pipe_count = 0
        self.reconnect_count = 0
        self._is_open = True
        self._read_event = asyncio.Event()

    def queue_response(self, data: bytes) -> None:
        if len(data) >= 4:
            cmd_id = struct.unpack("<I", data[:4])[0]
            if cmd_id == 9999:
                self.responses.extend(data)
                self._read_event.set()
                return
        self.pending_replies.append(data)

    async def read_exact(self, n: int, timeout: float | None = None) -> bytes:
        while len(self.responses) < n:
            self._read_event.clear()
            if timeout is not None:
                await asyncio.wait_for(self._read_event.wait(), timeout=timeout)
            else:
                await self._read_event.wait()
        data = bytes(self.responses[:n])
        del self.responses[:n]
        return data

    async def read(self, n: int) -> bytes:
        return await self.read_exact(n)

    async def write(self, data: bytes) -> None:
        if self.broken_pipe_count > 0:
            self.broken_pipe_count -= 1
            self.reconnect_count += 1
            raise BrokenPipeError("Mock broken pipe")
        self.written.append(data)
        if self.pending_replies:
            reply = self.pending_replies.pop(0)
            self.responses.extend(reply)
            self._read_event.set()

    async def close(self) -> None:
        self._is_open = False

    @property
    def is_open(self) -> bool:
        return self._is_open

