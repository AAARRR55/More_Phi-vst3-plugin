#!/usr/bin/env python3
"""Capture and diff hosted Ozone parameter changes around a manual Assistant run.

This is the production-safe path when iZotope's public inter-plugin
communication does not expose a confirmed writable shared-memory ring.

Typical workflow:
  python tools/ozone_assistant_diff.py capture --port 30001 --token "..." --out before.json
  # Run Ozone Assistant manually in the Ozone UI while FL is playing.
  python tools/ozone_assistant_diff.py capture --port 30001 --token "..." --out after.json
  python tools/ozone_assistant_diff.py diff --before before.json --after after.json --out assistant_diff.json
  python tools/ozone_assistant_diff.py apply --port 30001 --token "..." --diff assistant_diff.json
"""

from __future__ import annotations

import argparse
import json
import math
import socket
import sys
import time
from pathlib import Path
from typing import Any


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30001
DEFAULT_TIMEOUT_SECONDS = 30.0
DEFAULT_EPSILON = 1.0e-6


class JsonRpcClient:
    def __init__(self, host: str, port: int, token: str, timeout: float) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self.buffer = b""
        self.next_id = 1

    def __enter__(self) -> "JsonRpcClient":
        self.sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        self.sock.settimeout(self.timeout)
        response = self.request("initialize", {"bearer_token": self.token})
        if "error" in response:
            raise RuntimeError(f"initialize failed: {response['error']}")
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def request(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        if self.sock is None:
            raise RuntimeError("client is not connected")

        req_id = self.next_id
        self.next_id += 1
        message: dict[str, Any] = {"jsonrpc": "2.0", "method": method, "id": req_id}
        if params is not None:
            message["params"] = params

        self.sock.sendall((json.dumps(message, separators=(",", ":")) + "\n").encode("utf-8"))

        deadline = time.monotonic() + self.timeout
        while True:
            response = self._read_message(deadline)
            if response.get("id") == req_id:
                return response
            # MCP notifications, including notifications/initialized, have no id.

    def tool_call(self, name: str, arguments: dict[str, Any] | None = None) -> dict[str, Any]:
        return self.request("tools/call", {"name": name, "arguments": arguments or {}})

    def _read_message(self, deadline: float) -> dict[str, Any]:
        if self.sock is None:
            raise RuntimeError("client is not connected")

        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("timed out waiting for JSON-RPC response")
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(1024 * 1024)
            if not chunk:
                raise ConnectionError("MCP socket closed")
            self.buffer += chunk

        line, self.buffer = self.buffer.split(b"\n", 1)
        if not line.strip():
            return self._read_message(deadline)
        return json.loads(line.decode("utf-8", errors="replace"))


def unwrap_tool_result(response: dict[str, Any]) -> Any:
    if "error" in response:
        raise RuntimeError(f"JSON-RPC error: {response['error']}")

    result = response.get("result")
    if not isinstance(result, dict):
        return result

    if result.get("isError"):
        structured = result.get("structuredContent")
        if structured is not None:
            raise RuntimeError(f"tool error: {structured}")
        raise RuntimeError(f"tool error: {result}")

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


def parameter_key(parameter: dict[str, Any]) -> str:
    stable_id = parameter.get("stableId") or parameter.get("stable_id")
    if stable_id not in (None, ""):
        return f"stable:{stable_id}"
    return f"index:{int(parameter.get('index', parameter.get('id')))}"


def numeric_value(parameter: dict[str, Any]) -> float:
    value = float(parameter.get("value"))
    if not math.isfinite(value):
        raise ValueError(f"non-finite parameter value for {parameter.get('name')!r}: {value}")
    return value


def capture(args: argparse.Namespace) -> int:
    with JsonRpcClient(args.host, args.port, args.token, args.timeout) as client:
        info = unwrap_tool_result(client.tool_call("get_plugin_info"))
        parameters = unwrap_tool_result(client.tool_call("list_parameters"))

    if not isinstance(parameters, list):
        raise RuntimeError("list_parameters did not return an array")

    payload = {
        "schema_version": 1,
        "captured_at_unix": time.time(),
        "source": "more_phi_hosted_plugin_parameters",
        "host": args.host,
        "port": args.port,
        "plugin_info": info,
        "parameter_count": len(parameters),
        "parameters": parameters,
    }
    write_json(args.out, payload)
    print(f"captured {len(parameters)} parameters -> {args.out}")
    if isinstance(info, dict):
        hosted = info.get("hostedPlugin")
        if isinstance(hosted, dict):
            print(f"hosted plugin: {hosted.get('name')} ({hosted.get('paramCount')} params)")
    return 0


def diff(args: argparse.Namespace) -> int:
    before = read_json(args.before)
    after = read_json(args.after)
    before_params = before.get("parameters")
    after_params = after.get("parameters")
    if not isinstance(before_params, list) or not isinstance(after_params, list):
        raise RuntimeError("both snapshots must contain a parameters array")

    before_by_key = {parameter_key(p): p for p in before_params if isinstance(p, dict)}
    changes: list[dict[str, Any]] = []

    for after_param in after_params:
        if not isinstance(after_param, dict):
            continue
        key = parameter_key(after_param)
        before_param = before_by_key.get(key)
        if before_param is None:
            continue
        before_value = numeric_value(before_param)
        after_value = numeric_value(after_param)
        delta = after_value - before_value
        if abs(delta) <= args.epsilon:
            continue

        changes.append({
            "index": int(after_param.get("index", after_param.get("id"))),
            "id": int(after_param.get("id", after_param.get("index"))),
            "stableId": str(after_param.get("stableId", after_param.get("stable_id", ""))),
            "name": str(after_param.get("name", "")),
            "label": str(after_param.get("label", "")),
            "before": before_value,
            "after": after_value,
            "value": after_value,
            "delta": delta,
            "beforeDisplayValue": str(before_param.get("displayValue", "")),
            "afterDisplayValue": str(after_param.get("displayValue", "")),
            "discrete": bool(after_param.get("discrete", False)),
            "boolean": bool(after_param.get("boolean", False)),
        })

    payload = {
        "schema_version": 1,
        "source": "ozone_assistant_parameter_diff",
        "before": str(args.before),
        "after": str(args.after),
        "epsilon": args.epsilon,
        "changed_count": len(changes),
        "changes": changes,
        "parameters": [
            {
                "index": change["index"],
                "stableId": change["stableId"],
                "name": change["name"],
                "value": change["value"],
            }
            for change in changes
        ],
    }
    write_json(args.out, payload)
    print(f"diff found {len(changes)} changed parameters -> {args.out}")
    return 0


def apply_diff(args: argparse.Namespace) -> int:
    payload = read_json(args.diff)
    parameters = payload.get("parameters")
    if not isinstance(parameters, list):
        raise RuntimeError("diff file does not contain a replayable parameters array")
    if not parameters:
        print("diff contains no parameter changes; nothing to apply")
        return 0

    with JsonRpcClient(args.host, args.port, args.token, args.timeout) as client:
        response = unwrap_tool_result(client.tool_call(
            "set_parameters_batch",
            {"parameters": parameters},
        ))

    print(json.dumps(response, indent=2, ensure_ascii=False))
    return 0


def read_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise RuntimeError(f"{path} does not contain a JSON object")
    return data


def write_json(path: Path, data: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Capture, diff, and replay hosted Ozone parameter changes around a manual Assistant run.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    def add_connection_options(sub: argparse.ArgumentParser) -> None:
        sub.add_argument("--host", default=DEFAULT_HOST)
        sub.add_argument("--port", type=int, default=DEFAULT_PORT)
        sub.add_argument("--token", required=True, help="More-Phi MCP bearer token.")
        sub.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)

    capture_parser = subparsers.add_parser("capture", help="Capture current hosted plugin parameters.")
    add_connection_options(capture_parser)
    capture_parser.add_argument("--out", type=Path, required=True)
    capture_parser.set_defaults(func=capture)

    diff_parser = subparsers.add_parser("diff", help="Diff two captured parameter snapshots.")
    diff_parser.add_argument("--before", type=Path, required=True)
    diff_parser.add_argument("--after", type=Path, required=True)
    diff_parser.add_argument("--out", type=Path, required=True)
    diff_parser.add_argument("--epsilon", type=float, default=DEFAULT_EPSILON)
    diff_parser.set_defaults(func=diff)

    apply_parser = subparsers.add_parser("apply", help="Replay a diff through More-Phi's parameter queue.")
    add_connection_options(apply_parser)
    apply_parser.add_argument("--diff", type=Path, required=True)
    apply_parser.set_defaults(func=apply_diff)

    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return int(args.func(args))
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
