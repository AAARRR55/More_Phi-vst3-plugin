"""
Append-only JSONL audit logger for MCP tool calls.
"""

from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


DEFAULT_AUDIT_DIR = Path.home() / ".mcp_vst3"
DEFAULT_AUDIT_FILE = DEFAULT_AUDIT_DIR / "audit.jsonl"


@dataclass
class AuditLogger:
    path: Path = field(default_factory=lambda: DEFAULT_AUDIT_FILE)

    def __post_init__(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)

    def log(
        self,
        request_id: str,
        tool: str,
        arguments: dict[str, Any],
        result: dict[str, Any],
        latency_ms: float,
    ) -> None:
        entry = {
            "ts": _iso_now(),
            "request_id": request_id,
            "tool": tool,
            "input": arguments,
            "result": result,
            "latency_ms": latency_ms,
        }
        try:
            with open(self.path, "a", encoding="utf-8") as f:
                f.write(json.dumps(entry, default=str) + "\n")
        except Exception:
            # Audit logging must never fail a tool call.
            pass


def _iso_now() -> str:
    from datetime import datetime, timezone
    return datetime.now(timezone.utc).isoformat()
