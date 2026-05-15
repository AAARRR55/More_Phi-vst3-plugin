"""
agent_client.py — Python MCP client for the Ozone Track Assistant MCP server.

Usage (requires the Node.js server to be built first):
    cd scripts/ozone-mcp-server && npm install && npm run build && cd ../..
    OZONE_API_KEY=... OZONE_BASE_URL=... python scripts/agent_client.py

The client spawns the MCP server as a subprocess and communicates over stdio
using JSON-RPC 2.0, following the MCP initialize → tool-call lifecycle.
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
from queue import Queue, Empty
from typing import Any


class OzoneMCPClient:
    """Minimal MCP client communicating with the Ozone MCP server via stdio."""

    def __init__(self, server_command: list[str], env: dict[str, str]) -> None:
        self._proc = subprocess.Popen(
            server_command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        self._request_id = 0
        self._pending: dict[int, Queue[dict[str, Any]]] = {}
        self._lock = threading.Lock()

        # Background thread reads server responses
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()

        self._initialize()

    # ── Internal helpers ──────────────────────────────────────────────────────

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

    def _rpc(self, method: str, params: dict[str, Any], timeout: float = 30.0) -> dict[str, Any]:
        req_id = self._next_id()
        q: Queue[dict[str, Any]] = Queue()
        with self._lock:
            self._pending[req_id] = q
        self._send({"jsonrpc": "2.0", "id": req_id, "method": method, "params": params})
        try:
            response = q.get(timeout=timeout)
        except Empty:
            raise TimeoutError(f"No response for request {req_id} within {timeout}s")
        finally:
            with self._lock:
                self._pending.pop(req_id, None)
        return response

    def _initialize(self) -> None:
        self._rpc(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "OzonePythonAgent", "version": "1.0.0"},
            },
        )
        # Send initialized notification (no response expected)
        self._send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    # ── Public API ────────────────────────────────────────────────────────────

    def call_tool(self, tool_name: str, arguments: dict[str, Any]) -> Any:
        """
        Invoke an MCP tool and return the parsed JSON payload.

        Raises RuntimeError on protocol errors or tool-level errors.
        """
        response = self._rpc("tools/call", {"name": tool_name, "arguments": arguments})

        if "error" in response:
            raise RuntimeError(f"Protocol error: {response['error']}")

        result = response["result"]
        if result.get("isError"):
            raise RuntimeError(f"Tool error: {result['content'][0]['text']}")

        return json.loads(result["content"][0]["text"])

    def list_tools(self) -> list[dict[str, Any]]:
        """Return the server's tool manifest."""
        response = self._rpc("tools/list", {})
        return response["result"]["tools"]

    def close(self) -> None:
        if self._proc.stdin:
            self._proc.stdin.close()
        self._proc.wait(timeout=5)


# ── Demo workflow ─────────────────────────────────────────────────────────────

def main() -> None:
    server_dir = os.path.join(os.path.dirname(__file__), "ozone-mcp-server")
    server_cmd = ["node", os.path.join(server_dir, "dist", "index.js")]

    env = {
        **os.environ,
        "OZONE_API_KEY":  os.environ.get("OZONE_API_KEY", ""),
        "OZONE_BASE_URL": os.environ.get("OZONE_BASE_URL", ""),
    }

    if not env["OZONE_API_KEY"] or not env["OZONE_BASE_URL"]:
        print(
            "Set OZONE_API_KEY and OZONE_BASE_URL environment variables before running.\n"
            "Example:\n"
            "  OZONE_API_KEY=ozk_prod_xxx OZONE_BASE_URL=https://... python scripts/agent_client.py"
        )
        return

    client = OzoneMCPClient(server_command=server_cmd, env=env)

    try:
        # ── Step 1: Discover available tools ─────────────────────────────────
        tools = client.list_tools()
        print(f"Available tools: {[t['name'] for t in tools]}\n")

        # ── Step 2: Search for a track by title ───────────────────────────────
        print("Searching for 'Midnight Drive' ...")
        search_result = client.call_tool("ozone_track_search", {
            "query": "Midnight Drive",
            "page_size": 5,
        })
        if not search_result.get("results"):
            print("No tracks found.")
            return

        track_id: str = search_result["results"][0]["track_id"]
        title: str    = search_result["results"][0]["title"]
        print(f"Found: {title} ({track_id})\n")

        # ── Step 3: Run streaming loudness analysis ───────────────────────────
        print("Analyzing loudness (streaming profile) ...")
        analysis = client.call_tool("ozone_track_analyze", {
            "track_id": track_id,
            "analysis_profile": "streaming",
        })
        an = analysis["analysis"]
        lufs        = an["lufs_integrated"]
        true_peak   = an["true_peak_dbtp"]
        dyn_range   = an["dynamic_range_db"]
        rec         = an.get("ozone_recommendation", "")

        spotify_target = -14.0
        delta = lufs - spotify_target

        print(f"  LUFS integrated : {lufs:+.1f}  (Spotify target: {spotify_target:+.1f}, delta: {delta:+.1f})")
        print(f"  True peak       : {true_peak:+.1f} dBTP  (recommended ≤ -1.0)")
        print(f"  Dynamic range   : {dyn_range:.1f} dB")
        print(f"  Ozone note      : {rec}\n")

        # ── Step 4: Approve or flag the track based on analysis ───────────────
        if abs(delta) < 0.5 and true_peak <= -1.0:
            new_status = "mastering_complete"
            reason: str | None = None
            print("Track meets streaming standards → marking mastering_complete")
        else:
            issues = []
            if abs(delta) >= 0.5:
                issues.append(f"LUFS delta {delta:+.1f} vs Spotify target")
            if true_peak > -1.0:
                issues.append(f"true peak {true_peak:+.1f} dBTP exceeds -1.0 dBTP")
            new_status = "on_hold"
            reason = "Requires adjustment: " + "; ".join(issues)
            print(f"Track needs work → setting on_hold ({reason})")

        update_result = client.call_tool("ozone_track_update_status", {
            "track_id": track_id,
            "new_status": new_status,
            **({"reason": reason} if reason else {}),
        })
        print(f"  Status updated: {update_result}")

    finally:
        client.close()


if __name__ == "__main__":
    main()
