#!/usr/bin/env python3
"""Observe and optionally override Relay parameters changed by iZotope linking.

This is the safe interception layer: it does not decode or mutate private
iZotope IPC. It watches Relay's exposed VST parameters through More-Phi MCP and
can apply a policy back through More-Phi's normal `set_parameters_batch` tool.

Default mode is dry-run. Use --apply to actually write overrides.
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


RELAY_PARAM_INDICES = {
    "gain": 2,
    "pan": 3,
    "width": 4,
}


@dataclass
class Endpoint:
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
        init = self.request("initialize", {"bearer_token": self.endpoint.token})
        if "error" in init:
            raise RuntimeError(f"initialize failed: {init['error']}")
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        if self.sock is None:
            raise RuntimeError("not connected")
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


def clamp01(value: float) -> float:
    return max(0.0, min(1.0, value))


def param_map(parameters: list[dict[str, Any]]) -> dict[int, dict[str, Any]]:
    return {int(p["index"]): p for p in parameters if "index" in p}


def changed_indices(previous: dict[int, dict[str, Any]], current: dict[int, dict[str, Any]], epsilon: float) -> list[int]:
    changed: list[int] = []
    for index in RELAY_PARAM_INDICES.values():
        before = previous.get(index)
        after = current.get(index)
        if before is None or after is None:
            continue
        if abs(float(before.get("value", 0.0)) - float(after.get("value", 0.0))) > epsilon:
            changed.append(index)
    return changed


def desired_overrides(args: argparse.Namespace, current: dict[int, dict[str, Any]], changed: list[int]) -> list[dict[str, Any]]:
    overrides: list[dict[str, Any]] = []
    requested = {
        RELAY_PARAM_INDICES["gain"]: args.hold_gain,
        RELAY_PARAM_INDICES["pan"]: args.hold_pan,
        RELAY_PARAM_INDICES["width"]: args.hold_width,
    }
    clamps = {
        RELAY_PARAM_INDICES["gain"]: (args.min_gain, args.max_gain),
        RELAY_PARAM_INDICES["pan"]: (args.min_pan, args.max_pan),
        RELAY_PARAM_INDICES["width"]: (args.min_width, args.max_width),
    }

    for index in changed:
        param = current.get(index)
        if not param:
            continue
        current_value = float(param.get("value", 0.0))
        target = requested.get(index)
        if target is None:
            low, high = clamps[index]
            target = min(max(current_value, low), high)
        target = clamp01(float(target))
        if abs(target - current_value) > args.epsilon:
            overrides.append({"index": index, "value": target})
    return overrides


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--token", required=True)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--epsilon", type=float, default=1.0e-7)
    parser.add_argument("--apply", action="store_true", help="Actually write overrides. Default is dry-run.")
    parser.add_argument("--hold-gain", type=float, help="Force Relay Global Output Gain normalized value.")
    parser.add_argument("--hold-pan", type=float, help="Force Relay Pan normalized value.")
    parser.add_argument("--hold-width", type=float, help="Force Relay Width normalized value.")
    parser.add_argument("--min-gain", type=float, default=0.0)
    parser.add_argument("--max-gain", type=float, default=1.0)
    parser.add_argument("--min-pan", type=float, default=0.0)
    parser.add_argument("--max-pan", type=float, default=1.0)
    parser.add_argument("--min-width", type=float, default=0.0)
    parser.add_argument("--max-width", type=float, default=1.0)
    parser.add_argument("--cooldown", type=float, default=0.25, help="Minimum seconds between override writes.")
    parser.add_argument("--out-dir", default="tools/live_captures/linked_instances")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    jsonl_path = out_dir / f"relay_link_interceptor_{stamp}.jsonl"
    summary_path = out_dir / f"relay_link_interceptor_{stamp}_summary.json"

    mode = "APPLY" if args.apply else "DRY-RUN"
    print(f"{mode} Relay link interceptor on port {args.port}")
    print(f"Writing events to {jsonl_path}")

    endpoint = Endpoint(args.host, args.port, args.token)
    writes = 0
    observed = 0
    last_write = 0.0
    first_snapshot: dict[int, dict[str, Any]] | None = None
    final_snapshot: dict[int, dict[str, Any]] | None = None

    with JsonRpcSession(endpoint) as session, jsonl_path.open("w", encoding="utf-8") as fp:
        initial = session.tool("get_plugin_info")
        print(json.dumps(initial, indent=2)[:1200])
        previous_raw = session.tool("list_parameters")
        if not isinstance(previous_raw, list):
            raise RuntimeError(f"list_parameters failed: {previous_raw}")
        previous = param_map(previous_raw)
        first_snapshot = previous
        start = time.monotonic()
        while time.monotonic() - start < args.duration:
            time.sleep(max(0.05, args.interval))
            current_raw = session.tool("list_parameters")
            if not isinstance(current_raw, list):
                event = {"elapsed": time.monotonic() - start, "error": current_raw}
                fp.write(json.dumps(event, separators=(",", ":")) + "\n")
                fp.flush()
                continue
            current = param_map(current_raw)
            final_snapshot = current
            changed = changed_indices(previous, current, args.epsilon)
            overrides = desired_overrides(args, current, changed) if changed else []
            if changed:
                observed += len(changed)
                detail = [
                    {
                        "index": index,
                        "name": current[index].get("name"),
                        "old": previous[index].get("value"),
                        "new": current[index].get("value"),
                        "oldDisplay": previous[index].get("displayValue"),
                        "newDisplay": current[index].get("displayValue"),
                    }
                    for index in changed
                    if index in previous and index in current
                ]
                print(f"{time.monotonic() - start:.2f}s observed {detail}; overrides={overrides}")
            write_response = None
            now = time.monotonic()
            if args.apply and overrides and now - last_write >= args.cooldown:
                write_response = session.tool("set_parameters_batch", {"parameters": overrides})
                last_write = now
                writes += len(overrides)
            event = {
                "elapsed": time.monotonic() - start,
                "changed_indices": changed,
                "overrides": overrides,
                "applied": bool(args.apply and overrides),
                "write_response": write_response,
                "relay_params": {name: current[index] for name, index in RELAY_PARAM_INDICES.items() if index in current},
            }
            fp.write(json.dumps(event, separators=(",", ":")) + "\n")
            fp.flush()
            previous = current

    def compact(snapshot: dict[int, dict[str, Any]] | None) -> dict[str, Any]:
        if not snapshot:
            return {}
        return {
            name: {
                "index": index,
                "value": snapshot[index].get("value"),
                "displayValue": snapshot[index].get("displayValue"),
            }
            for name, index in RELAY_PARAM_INDICES.items()
            if index in snapshot
        }

    summary = {
        "mode": mode,
        "jsonl_path": str(jsonl_path),
        "duration": args.duration,
        "observed_changes": observed,
        "override_writes": writes,
        "initial": compact(first_snapshot),
        "final": compact(final_snapshot),
    }
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Summary written to {summary_path}")
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
