#!/usr/bin/env python3
"""Poll two authenticated More-Phi instances and log linked-plugin changes.

This is a local JSON-RPC client for More-Phi MCP endpoints. It does not touch
iZotope internals directly; it records the observable hosted-plugin state while
separate Frida read-only traces watch iZotope code/data paths.
"""

from __future__ import annotations

import argparse
import json
import socket
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


@dataclass
class Endpoint:
    name: str
    host: str
    port: int
    token: str


class JsonRpcSession:
    def __init__(self, endpoint: Endpoint, timeout: float = 10.0) -> None:
        self.endpoint = endpoint
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self.buffer = b""
        self.next_id = 0

    def __enter__(self) -> "JsonRpcSession":
        self.sock = socket.create_connection((self.endpoint.host, self.endpoint.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        self.request("initialize", {"bearer_token": self.endpoint.token})
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        if self.sock is None:
            raise RuntimeError("session is not connected")
        self.next_id += 1
        req_id = self.next_id
        message: dict[str, Any] = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            message["params"] = params
        self.sock.sendall((json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8"))
        deadline = time.monotonic() + self.timeout
        while True:
            while b"\n" not in self.buffer:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError(f"timed out waiting for {method}")
                self.sock.settimeout(remaining)
                chunk = self.sock.recv(1024 * 1024)
                if not chunk:
                    raise ConnectionError("MCP socket closed")
                self.buffer += chunk
            line, self.buffer = self.buffer.split(b"\n", 1)
            if not line.strip():
                continue
            response = json.loads(line.decode("utf-8", errors="replace"))
            if response.get("id") == req_id:
                return response

    def tool(self, name: str, arguments: dict[str, Any] | None = None) -> Any:
        return unwrap_tool_result(self.request("tools/call", {"name": name, "arguments": arguments or {}}))


def unwrap_tool_result(response: dict[str, Any]) -> Any:
    if "error" in response:
        return {"jsonrpc_error": response["error"]}
    result = response.get("result")
    if not isinstance(result, dict):
        return result
    if "structuredContent" in result:
        return result["structuredContent"]
    content = result.get("content")
    if isinstance(content, list) and content:
        text = content[0].get("text") if isinstance(content[0], dict) else None
        if isinstance(text, str):
            try:
                return json.loads(text)
            except json.JSONDecodeError:
                return text
    return result


def param_changes(previous: list[dict[str, Any]] | None, current: list[dict[str, Any]], epsilon: float) -> list[dict[str, Any]]:
    if previous is None:
        return []
    prev_by_index = {p.get("index"): p for p in previous}
    changes: list[dict[str, Any]] = []
    for param in current:
        index = param.get("index")
        before = prev_by_index.get(index)
        if before is None:
            changes.append({"kind": "added", "index": index, "name": param.get("name"), "new": param.get("value")})
            continue
        old_value = float(before.get("value", 0.0))
        new_value = float(param.get("value", 0.0))
        old_display = before.get("displayValue")
        new_display = param.get("displayValue")
        if abs(new_value - old_value) > epsilon or new_display != old_display:
            changes.append(
                {
                    "kind": "changed",
                    "index": index,
                    "name": param.get("name"),
                    "old": old_value,
                    "new": new_value,
                    "oldDisplay": old_display,
                    "newDisplay": new_display,
                }
            )
    return changes


def parse_endpoint(raw: str) -> Endpoint:
    # name:port:token, where token may contain spaces if quoted by the shell.
    parts = raw.split(":", 2)
    if len(parts) != 3:
        raise argparse.ArgumentTypeError("endpoint must be name:port:token")
    name, port_text, token = parts
    return Endpoint(name=name, host="127.0.0.1", port=int(port_text), token=token.strip())


def capture_endpoint(session: JsonRpcSession, parameters_only: bool) -> dict[str, Any]:
    data: dict[str, Any] = {}
    data["parameters"] = session.tool("list_parameters")
    if not parameters_only:
        data["plugin_info"] = session.tool("get_plugin_info")
        data["morph_state"] = session.tool("get_morph_state")
        data["analysis"] = session.tool("analysis.capture_window", {"window_ms": 1000})
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--endpoint", action="append", type=parse_endpoint, required=True, help="name:port:token")
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--epsilon", type=float, default=1.0e-7)
    parser.add_argument("--parameters-only", action="store_true", help="Only poll list_parameters to avoid MCP rate limits.")
    parser.add_argument("--out-dir", default="tools/live_captures/linked_instances")
    args = parser.parse_args()

    if len(args.endpoint) < 1:
        raise SystemExit("At least one endpoint is required.")

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    jsonl_path = out_dir / f"linked_instance_monitor_{stamp}.jsonl"
    summary_path = out_dir / f"linked_instance_monitor_{stamp}_summary.json"

    print(f"Writing monitor events to {jsonl_path}")
    print("Move linked plugin controls or trigger Assistant while this runs.")

    sessions: list[JsonRpcSession] = []
    previous_params: dict[str, list[dict[str, Any]] | None] = {}
    total_changes: dict[str, int] = {}
    samples: dict[str, list[dict[str, Any]]] = {}

    try:
        for endpoint in args.endpoint:
            session = JsonRpcSession(endpoint)
            session.__enter__()
            sessions.append(session)
            previous_params[endpoint.name] = None
            total_changes[endpoint.name] = 0
            samples[endpoint.name] = []

        start = time.monotonic()
        with jsonl_path.open("w", encoding="utf-8") as fp:
            iteration = 0
            while time.monotonic() - start < args.duration:
                iteration += 1
                for session in sessions:
                    endpoint = session.endpoint
                    try:
                        snapshot = capture_endpoint(session, args.parameters_only)
                        params = snapshot.get("parameters")
                        if not isinstance(params, list):
                            event = {
                                "t_wall": time.time(),
                                "t_monotonic": time.monotonic(),
                                "elapsed": time.monotonic() - start,
                                "iteration": iteration,
                                "endpoint": endpoint.name,
                                "port": endpoint.port,
                                "snapshot": snapshot,
                                "changes": [],
                                "parameter_read_valid": False,
                            }
                            fp.write(json.dumps(event, separators=(",", ":")) + "\n")
                            fp.flush()
                            continue
                        changes = param_changes(previous_params[endpoint.name], params, args.epsilon)
                        previous_params[endpoint.name] = params
                        if changes:
                            total_changes[endpoint.name] += len(changes)
                            samples[endpoint.name].extend(changes[:20])
                            print(f"{endpoint.name}:{endpoint.port} changes at {time.monotonic() - start:.2f}s: {changes}")
                        event = {
                            "t_wall": time.time(),
                            "t_monotonic": time.monotonic(),
                            "elapsed": time.monotonic() - start,
                            "iteration": iteration,
                            "endpoint": endpoint.name,
                            "port": endpoint.port,
                            "snapshot": snapshot,
                            "changes": changes,
                            "parameter_read_valid": True,
                        }
                    except Exception as exc:
                        event = {
                            "t_wall": time.time(),
                            "t_monotonic": time.monotonic(),
                            "elapsed": time.monotonic() - start,
                            "iteration": iteration,
                            "endpoint": endpoint.name,
                            "port": endpoint.port,
                            "error": str(exc),
                        }
                        print(f"{endpoint.name}:{endpoint.port} error: {exc}")
                    fp.write(json.dumps(event, separators=(",", ":")) + "\n")
                    fp.flush()
                time.sleep(max(0.05, args.interval))
    finally:
        for session in sessions:
            session.__exit__(None, None, None)

    summary = {
        "jsonl_path": str(jsonl_path),
        "duration": args.duration,
        "interval": args.interval,
        "parameters_only": args.parameters_only,
        "endpoints": [{"name": e.name, "port": e.port} for e in args.endpoint],
        "total_parameter_changes": total_changes,
        "sample_changes": {name: value[:40] for name, value in samples.items()},
    }
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Summary written to {summary_path}")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
