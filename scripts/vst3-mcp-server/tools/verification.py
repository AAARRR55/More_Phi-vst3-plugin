"""
Verification record formatting and corrective actions for tool results.
"""

from __future__ import annotations

import time
import uuid
from dataclasses import dataclass, field
from typing import Any, Literal


CORRECTIVE_ACTIONS = {
    "TIMEOUT": "Retry with smaller batch or check VST3 host responsiveness.",
    "PARAM_OUT_RANGE": "Clamp value to [{min}, {max}] and retry.",
    "PLUGIN_NOT_READY": "Wait for plugin initialization; check IPC pipe connection.",
    "PIPE_BROKEN": "Reconnect IPC bridge: bridge.reconnect() or restart the server.",
    "INVALID_PARAM_ID": "Call list_parameters to refresh the parameter map.",
}


def build_corrective_action(error_code: str, context: dict[str, Any] | None = None) -> str:
    template = CORRECTIVE_ACTIONS.get(error_code, "Check VST3 host logs for details.")
    try:
        return template.format(**(context or {}))
    except (KeyError, ValueError):
        return template


@dataclass
class VerificationRecord:
    request_id: str
    tool_name: str
    input_params: dict[str, Any]
    param_id: int
    value_before: float
    value_after: float
    human_before: str
    human_after: str
    execution_time_ms: float
    status: Literal["success", "failure", "timeout"]
    error_reason: str | None
    corrective_action: str | None
    timestamp: str = field(default_factory=lambda: _iso_now())

    def to_tool_result(self) -> dict[str, Any]:
        return {
            "status": self.status,
            "param_id": self.param_id,
            "value_before": self.value_before,
            "value_after": self.value_after,
            "human_before": self.human_before,
            "human_after": self.human_after,
            "error_message": self.error_reason,
            "corrective_action": self.corrective_action,
            "execution_time_ms": self.execution_time_ms,
            "timestamp": self.timestamp,
        }

    def to_audit_entry(self) -> dict[str, Any]:
        return {
            "request_id": self.request_id,
            "tool": self.tool_name,
            "input": self.input_params,
            "param_id": self.param_id,
            "value_before": self.value_before,
            "value_after": self.value_after,
            "status": self.status,
            "error_reason": self.error_reason,
            "execution_time_ms": self.execution_time_ms,
            "timestamp": self.timestamp,
        }


def _iso_now() -> str:
    from datetime import datetime, timezone
    return datetime.now(timezone.utc).isoformat()


def make_request_id() -> str:
    return f"req_{uuid.uuid4().hex[:8]}"
