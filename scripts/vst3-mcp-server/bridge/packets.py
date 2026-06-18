"""
Binary packet serialization for the More-Phi VST3 IPC bridge.

This module defines the exact byte layout shared with the C++ side
(src/AI/VST3IPCBridge.h). All multi-byte integers are little-endian.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Self


class CommandType(IntEnum):
    SET_PARAM = 1
    LOAD_PRESET = 2
    GET_STATE = 3
    BATCH = 4


class ResultStatus(IntEnum):
    SUCCESS = 0
    FAILURE = 1
    TIMEOUT = 2


COMMAND_HEADER_FORMAT = "<IBIdI"  # command_id, command_type, param_id, normalized_value, payload_length
COMMAND_HEADER_SIZE = struct.calcsize(COMMAND_HEADER_FORMAT)
RESULT_HEADER_FORMAT = "<IBddQI"  # command_id, status, value_before, value_after, timestamp_ns, payload_length
RESULT_HEADER_SIZE = struct.calcsize(RESULT_HEADER_FORMAT)
BATCH_PAIR_SIZE = struct.calcsize("<Id")  # param_id, normalized_value


@dataclass
class CommandPacketHeader:
    command_id: int = 0
    command_type: int = 0
    param_id: int = 0
    normalized_value: float = 0.0
    payload_length: int = 0

    def serialize(self) -> bytes:
        return struct.pack(
            COMMAND_HEADER_FORMAT,
            self.command_id,
            self.command_type,
            self.param_id,
            self.normalized_value,
            self.payload_length,
        )

    @classmethod
    def deserialize(cls, data: bytes) -> Self:
        if len(data) < COMMAND_HEADER_SIZE:
            raise ValueError(f"Command header too short: {len(data)} < {COMMAND_HEADER_SIZE}")
        parts = struct.unpack(COMMAND_HEADER_FORMAT, data[:COMMAND_HEADER_SIZE])
        return cls(*parts)


@dataclass
class ResultPacketHeader:
    command_id: int = 0
    status: int = 0
    value_before: float = 0.0
    value_after: float = 0.0
    timestamp_ns: int = 0
    payload_length: int = 0

    def serialize(self) -> bytes:
        return struct.pack(
            RESULT_HEADER_FORMAT,
            self.command_id,
            self.status,
            self.value_before,
            self.value_after,
            self.timestamp_ns,
            self.payload_length,
        )

    @classmethod
    def deserialize(cls, data: bytes) -> Self:
        if len(data) < RESULT_HEADER_SIZE:
            raise ValueError(f"Result header too short: {len(data)} < {RESULT_HEADER_SIZE}")
        parts = struct.unpack(RESULT_HEADER_FORMAT, data[:RESULT_HEADER_SIZE])
        return cls(*parts)


@dataclass
class CommandPacket:
    header: CommandPacketHeader = field(default_factory=CommandPacketHeader)
    payload: bytes = b""

    def serialize(self) -> bytes:
        self.header.payload_length = len(self.payload)
        return self.header.serialize() + self.payload

    @classmethod
    def from_header_and_payload(cls, header: CommandPacketHeader, payload: bytes = b"") -> Self:
        header.payload_length = len(payload)
        return cls(header=header, payload=payload)

    @classmethod
    def set_param(cls, command_id: int, param_id: int, normalized_value: float) -> Self:
        return cls(
            header=CommandPacketHeader(
                command_id=command_id,
                command_type=CommandType.SET_PARAM,
                param_id=param_id,
                normalized_value=normalized_value,
            )
        )

    @classmethod
    def get_state(cls, command_id: int) -> Self:
        return cls(
            header=CommandPacketHeader(
                command_id=command_id,
                command_type=CommandType.GET_STATE,
            )
        )

    @classmethod
    def load_preset(cls, command_id: int, preset_name: str) -> Self:
        payload = preset_name.encode("utf-8")
        return cls(
            header=CommandPacketHeader(
                command_id=command_id,
                command_type=CommandType.LOAD_PRESET,
                payload_length=len(payload),
            ),
            payload=payload,
        )

    @classmethod
    def batch(cls, command_id: int, params: list[tuple[int, float]]) -> Self:
        """Build a BATCH packet from a list of (param_id, normalized_value) pairs."""
        payload = b"".join(
            struct.pack("<Id", param_id, value) for param_id, value in params
        )
        return cls(
            header=CommandPacketHeader(
                command_id=command_id,
                command_type=CommandType.BATCH,
                payload_length=len(payload),
            ),
            payload=payload,
        )


def parse_batch_payload(payload: bytes) -> list[tuple[int, float]]:
    """Parse a BATCH payload into (param_id, normalized_value) pairs."""
    if len(payload) % BATCH_PAIR_SIZE != 0:
        raise ValueError("BATCH payload length is not a multiple of pair size")
    pairs: list[tuple[int, float]] = []
    for offset in range(0, len(payload), BATCH_PAIR_SIZE):
        param_id, value = struct.unpack_from("<Id", payload, offset)
        pairs.append((param_id, value))
    return pairs


# BATCH result diff payload: little-endian (uint32 param_id, double before, double after).
# Mirrors C++ VST3IPCBridge::serializeBatchDiffs (20 bytes per diff).
BATCH_DIFF_FORMAT = "<Idd"
BATCH_DIFF_SIZE = struct.calcsize(BATCH_DIFF_FORMAT)


def parse_batch_diffs(payload: bytes) -> list[tuple[int, float, float]]:
    """Parse a BATCH result diff payload into (param_id, before, after) triples."""
    if len(payload) % BATCH_DIFF_SIZE != 0:
        raise ValueError(f"Batch diff payload length is not a multiple of {BATCH_DIFF_SIZE}")
    return [
        struct.unpack_from(BATCH_DIFF_FORMAT, payload, offset)
        for offset in range(0, len(payload), BATCH_DIFF_SIZE)
    ]


@dataclass
class ResultPacket:
    header: ResultPacketHeader = field(default_factory=ResultPacketHeader)
    payload: bytes = b""

    def serialize(self) -> bytes:
        self.header.payload_length = len(self.payload)
        return self.header.serialize() + self.payload

    @property
    def is_success(self) -> bool:
        return self.header.status == ResultStatus.SUCCESS

    @property
    def error_message(self) -> str:
        return self.payload.decode("utf-8", errors="replace") if self.payload else ""
