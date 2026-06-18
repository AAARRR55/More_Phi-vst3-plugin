"""
Append-only JSONL audit logger for MCP tool calls.
"""

from __future__ import annotations

import json
import sys
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
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
        except Exception as exc:  # noqa: BLE001 - never crash the server over audit setup
            print(f"[audit] could not create audit dir {self.path.parent}: {exc}", file=sys.stderr)

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
        except Exception as exc:  # noqa: BLE001 - audit logging must never fail a tool call
            # Surface the failure (to stderr; stdout is reserved for JSON-RPC) but
            # do not propagate -- a tool call must not fail because the audit log
            # is unwritable.
            print(f"[audit] failed to write audit entry: {exc}", file=sys.stderr)


def _iso_now() -> str:
    from datetime import datetime, timezone
    return datetime.now(timezone.utc).isoformat()
