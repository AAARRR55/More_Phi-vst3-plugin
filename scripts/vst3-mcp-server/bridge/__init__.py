"""IPC bridge and packet serialization for the More-Phi VST3 MCP server."""

from .ipc_bridge import VST3IPCBridge, VST3IPCError
from .packets import (
    BATCH_PAIR_SIZE,
    COMMAND_HEADER_SIZE,
    RESULT_HEADER_SIZE,
    CommandPacket,
    CommandPacketHeader,
    CommandType,
    ResultPacket,
    ResultPacketHeader,
    ResultStatus,
    parse_batch_payload,
)
from .transport import default_endpoint

__all__ = [
    "VST3IPCBridge",
    "VST3IPCError",
    "BATCH_PAIR_SIZE",
    "COMMAND_HEADER_SIZE",
    "RESULT_HEADER_SIZE",
    "CommandPacket",
    "CommandPacketHeader",
    "CommandType",
    "ResultPacket",
    "ResultPacketHeader",
    "ResultStatus",
    "parse_batch_payload",
    "default_endpoint",
]
