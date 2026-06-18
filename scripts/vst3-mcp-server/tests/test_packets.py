"""Tests for the binary packet serialization layer."""

from __future__ import annotations

import struct

import pytest

from bridge.packets import (
    BATCH_DIFF_SIZE,
    BATCH_PAIR_SIZE,
    COMMAND_HEADER_SIZE,
    RESULT_HEADER_SIZE,
    CommandPacket,
    CommandPacketHeader,
    CommandType,
    ResultPacket,
    ResultPacketHeader,
    ResultStatus,
    parse_batch_diffs,
    parse_batch_payload,
)


def test_command_header_size() -> None:
    assert COMMAND_HEADER_SIZE == 21


def test_result_header_size() -> None:
    assert RESULT_HEADER_SIZE == 33


def test_batch_pair_size() -> None:
    assert BATCH_PAIR_SIZE == 12


def test_command_header_serialize_deserialize() -> None:
    header = CommandPacketHeader(
        command_id=0xA1B2C3D4,
        command_type=CommandType.BATCH,
        param_id=42,
        normalized_value=0.75,
        payload_length=0,
    )
    data = header.serialize()
    assert len(data) == COMMAND_HEADER_SIZE
    restored = CommandPacketHeader.deserialize(data)
    assert restored.command_id == header.command_id
    assert restored.command_type == header.command_type
    assert restored.param_id == header.param_id
    assert restored.normalized_value == pytest.approx(header.normalized_value)
    assert restored.payload_length == header.payload_length


def test_result_header_serialize_deserialize() -> None:
    header = ResultPacketHeader(
        command_id=7,
        status=ResultStatus.SUCCESS,
        value_before=0.1,
        value_after=0.9,
        timestamp_ns=1234567890123456789,
        payload_length=4,
    )
    data = header.serialize()
    assert len(data) == RESULT_HEADER_SIZE
    restored = ResultPacketHeader.deserialize(data)
    assert restored.command_id == header.command_id
    assert restored.status == header.status
    assert restored.value_before == pytest.approx(header.value_before)
    assert restored.value_after == pytest.approx(header.value_after)
    assert restored.timestamp_ns == header.timestamp_ns
    assert restored.payload_length == header.payload_length


def test_command_packet_with_payload() -> None:
    payload = b"hello"
    packet = CommandPacket.from_header_and_payload(
        CommandPacketHeader(command_id=1, command_type=CommandType.LOAD_PRESET),
        payload,
    )
    data = packet.serialize()
    assert len(data) == COMMAND_HEADER_SIZE + len(payload)
    header = CommandPacketHeader.deserialize(data)
    assert header.payload_length == len(payload)
    assert data[COMMAND_HEADER_SIZE:] == payload


def test_result_packet_with_payload() -> None:
    payload = b"done"
    packet = ResultPacket(
        header=ResultPacketHeader(
            command_id=1,
            status=ResultStatus.SUCCESS,
            payload_length=len(payload),
        ),
        payload=payload,
    )
    data = packet.serialize()
    assert len(data) == RESULT_HEADER_SIZE + len(payload)


def test_command_packet_helpers() -> None:
    cmd = CommandPacket.set_param(command_id=5, param_id=1001, normalized_value=0.5)
    assert cmd.header.command_type == CommandType.SET_PARAM
    assert cmd.header.param_id == 1001
    assert cmd.header.normalized_value == pytest.approx(0.5)

    cmd = CommandPacket.get_state(command_id=6)
    assert cmd.header.command_type == CommandType.GET_STATE

    cmd = CommandPacket.load_preset(command_id=7, preset_name="Warm Master")
    assert cmd.header.command_type == CommandType.LOAD_PRESET
    assert cmd.payload == b"Warm Master"

    cmd = CommandPacket.batch(command_id=8, params=[(1001, 0.25), (1002, 0.75)])
    assert cmd.header.command_type == CommandType.BATCH
    assert len(cmd.payload) == 2 * BATCH_PAIR_SIZE


def test_parse_batch_payload() -> None:
    params = [(1001, 0.25), (1002, 0.75)]
    payload = b"".join(struct.pack("<Id", pid, val) for pid, val in params)
    parsed = parse_batch_payload(payload)
    assert parsed == params


def test_parse_batch_payload_invalid_length() -> None:
    with pytest.raises(ValueError):
        parse_batch_payload(b"short")


def test_batch_diff_size() -> None:
    # Mirrors C++ VST3IPCBridge::kBatchDiffSize (uint32 + double + double).
    assert BATCH_DIFF_SIZE == 20


def test_parse_batch_diffs_matches_cpp_layout() -> None:
    # Cross-language contract: must match C++ serializeBatchDiffs (<Idd, 20 bytes).
    payload = (
        struct.pack("<Idd", 1001, 0.25, 0.5)
        + struct.pack("<Idd", 5001, 0.0, 0.75)
    )
    diffs = parse_batch_diffs(payload)
    assert len(diffs) == 2
    assert diffs[0] == (1001, pytest.approx(0.25), pytest.approx(0.5))
    assert diffs[1] == (5001, pytest.approx(0.0), pytest.approx(0.75))


def test_parse_batch_diffs_invalid_length() -> None:
    with pytest.raises(ValueError):
        parse_batch_diffs(b"short")
