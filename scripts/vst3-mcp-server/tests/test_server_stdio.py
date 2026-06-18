"""
Integration test that spawns the Python MCP server as a subprocess and
exercises initialize/tools/list over stdio.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time
from queue import Empty, Queue
from typing import Any

import pytest


class StdioMcpClient:
    """Minimal MCP client over stdio."""

    def __init__(self, server_script: str) -> None:
        env = {**os.environ, "PYTHONPATH": os.path.dirname(server_script)}
        self._proc = subprocess.Popen(
            [sys.executable, server_script],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        self._request_id = 0
        self._pending: dict[int, Queue[dict[str, Any]]] = {}
        self._lock = threading.Lock()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._initialize()

    def _next_id(self) -> int:
        with self._lock:
            self._request_id += 1
            return self._request_id

    def _send(self, payload: dict[str, Any]) -> None:
        assert self._proc.stdin is not None
        line = json.dumps(payload) + "\n"
        self._proc.stdin.write(line.encode())
        self._proc.stdin.flush()

    def _read_loop(self) -> None:
        assert self._proc.stdout is not None
        for raw_line in self._proc.stdout:
            try:
                msg: dict[str, Any] = json.loads(raw_line)
                req_id = msg.get("id")
                if req_id is not None:
                    with self._lock:
                        q = self._pending.get(req_id)
                    if q is not None:
                        q.put(msg)
            except json.JSONDecodeError:
                pass

    def _rpc(self, method: str, params: dict[str, Any], timeout: float = 10.0) -> dict[str, Any]:
        req_id = self._next_id()
        q: Queue[dict[str, Any]] = Queue()
        with self._lock:
            self._pending[req_id] = q
        self._send({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
        try:
            response = q.get(timeout=timeout)
        except Empty:
            raise TimeoutError(f"No response for request {req_id}")
        finally:
            with self._lock:
                self._pending.pop(req_id, None)
        return response

    def _initialize(self) -> None:
        response = self._rpc(
            "initialize",
            {
                "protocolVersion": "2025-06-18",
                "capabilities": {},
                "clientInfo": {"name": "TestClient", "version": "1.0.0"},
            },
        )
        if "error" in response:
            raise RuntimeError(f"Initialize failed: {response['error']}")
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    def list_tools(self) -> list[dict[str, Any]]:
        response = self._rpc("tools/list", {})
        return response["result"]["tools"]

    def call_tool(self, name: str, arguments: dict[str, Any]) -> dict[str, Any]:
        response = self._rpc("tools/call", {"name": name, "arguments": arguments})
        return response["result"]

    def close(self) -> None:
        if self._proc.stdin:
            try:
                self._proc.stdin.close()
            except Exception:
                pass
        self._proc.wait(timeout=5)


@pytest.fixture
def server_script() -> str:
    path = os.path.join(os.path.dirname(__file__), "..", "server.py")
    return os.path.abspath(path)


def test_server_initializes(server_script: str) -> None:
    client = StdioMcpClient(server_script)
    try:
        tools = client.list_tools()
        assert len(tools) > 0
        names = {t["name"] for t in tools}
        assert "set_eq_band" in names
        assert "apply_mastering_chain" in names
    finally:
        client.close()


def test_server_rejects_out_of_range_input(server_script: str) -> None:
    client = StdioMcpClient(server_script)
    try:
        result = client.call_tool(
            "set_eq_band",
            {"band_index": 0, "frequency_hz": 1000, "gain_db": 120.0, "q_factor": 1.0},
        )
        text = result["content"][0]["text"]
        try:
            content = json.loads(text)
            assert content["status"] != "success"
        except json.JSONDecodeError:
            assert "validation" in text.lower() or "greater than" in text.lower() or "maximum" in text.lower()
    finally:
        client.close()


def test_successful_call_returns_structured_content(server_script: str) -> None:
    """A successful tool call must return structuredContent (not the SDK
    'Output validation error: outputSchema defined but no structured output
    returned' rejection) and isError=False.

    get_spectrum_snapshot returns status=success without touching the IPC bridge,
    so this is a deterministic success path that does not depend on a running
    More-Phi host.
    """
    client = StdioMcpClient(server_script)
    try:
        result = client.call_tool("get_spectrum_snapshot", {})
        text = result["content"][0]["text"]
        assert "Output validation error" not in text, text
        assert result.get("structuredContent") is not None, result
        assert result.get("isError") is False, result
        assert result["structuredContent"]["status"] == "success"
    finally:
        client.close()


def test_handler_level_failure_sets_iserror_true(server_script: str) -> None:
    """When a tool resolves but the IPC bridge is unavailable, the server must
    surface a structured failure with isError=True (not mask it as success and
    not reject it as an output-validation error)."""
    client = StdioMcpClient(server_script)
    try:
        result = client.call_tool("set_output_gain", {"gain_db": -3.0})
        text = result["content"][0]["text"]
        assert "Output validation error" not in text, text
        assert result.get("structuredContent") is not None, result
        assert result.get("isError") is True, result
        assert result["structuredContent"]["status"] == "failure"
    finally:
        client.close()


def test_results_carry_verification_metadata(server_script: str) -> None:
    """Every result (success or failure) must carry uniform verification
    metadata (request_id, ISO-8601 timestamp, latency_ms) so the AI client can
    audit and correlate calls regardless of which handler ran (spec S5/S6)."""
    client = StdioMcpClient(server_script)
    try:
        result = client.call_tool("get_spectrum_snapshot", {})
        sc = result["structuredContent"]
        assert isinstance(sc.get("request_id"), str) and sc["request_id"].startswith("req_"), sc
        assert isinstance(sc.get("timestamp"), str) and "T" in sc["timestamp"], sc
        assert isinstance(sc.get("latency_ms"), (int, float)), sc
    finally:
        client.close()
