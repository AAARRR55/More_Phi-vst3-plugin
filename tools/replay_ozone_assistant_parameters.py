#!/usr/bin/env python3
"""Replay or synthesize captured Ozone Assistant parameter changes safely.

This tool does not decode, synthesize, or inject private iZotope IPC messages.
It replays the observable result of an Assistant run through More-Phi's normal
MCP hosted-plugin parameter surface (`set_parameters_batch`).

Default mode is dry-run. Use --apply with a More-Phi bearer token to write.
"""

from __future__ import annotations

import argparse
import json
import math
import socket
import sys
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 30001
DEFAULT_TIMEOUT_SECONDS = 30.0
DEFAULT_MONITOR = Path("tools/live_captures/linked_instances/linked_instance_monitor_20260516_043614.jsonl")
DEFAULT_OUT_DIR = Path("tools/live_captures/linked_instances")
DEFAULT_EPSILON = 1.0e-6


@dataclass
class AssistantChange:
    sequence: int
    burst_id: int
    elapsed: float
    index: int
    name: str
    old: float
    new: float
    old_display: str
    new_display: str


class JsonRpcClient:
    def __init__(self, host: str, port: int, token: str, timeout: float) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout
        self.sock: socket.socket | None = None
        self.buffer = b""
        self.next_id = 0

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


def clamp01(value: float) -> float:
    return max(0.0, min(1.0, value))


def finite_float(value: Any, field: str) -> float:
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"{field} is not finite: {value!r}")
    return number


def load_monitor_changes(path: Path, endpoint: str | None, epsilon: float) -> list[AssistantChange]:
    changes: list[AssistantChange] = []
    sequence = 0
    burst_id = 0

    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"{path}:{line_number}: invalid JSONL line: {exc}") from exc

            if endpoint and event.get("endpoint") != endpoint:
                continue

            raw_changes = event.get("changes")
            if not isinstance(raw_changes, list) or not raw_changes:
                continue

            burst_id += 1
            elapsed = finite_float(event.get("elapsed", 0.0), "elapsed")
            for raw in raw_changes:
                if not isinstance(raw, dict) or raw.get("kind") != "changed":
                    continue
                old = finite_float(raw.get("old"), "old")
                new = finite_float(raw.get("new"), "new")
                if abs(new - old) <= epsilon:
                    continue
                sequence += 1
                changes.append(
                    AssistantChange(
                        sequence=sequence,
                        burst_id=burst_id,
                        elapsed=elapsed,
                        index=int(raw["index"]),
                        name=str(raw.get("name", "")),
                        old=old,
                        new=new,
                        old_display=str(raw.get("oldDisplay", "")),
                        new_display=str(raw.get("newDisplay", "")),
                    )
                )

    return changes


def filter_changes(changes: list[AssistantChange], args: argparse.Namespace) -> list[AssistantChange]:
    only = set(args.only_index or [])
    exclude = set(args.exclude_index or [])
    name_filters = [needle.lower() for needle in (args.name_contains or [])]

    filtered: list[AssistantChange] = []
    for change in changes:
        if only and change.index not in only:
            continue
        if change.index in exclude:
            continue
        if name_filters and not any(needle in change.name.lower() for needle in name_filters):
            continue
        filtered.append(change)
    return filtered


def synthesize_value(old: float, new: float, blend: float) -> float:
    return clamp01(old + ((new - old) * blend))


def build_final_batches(changes: list[AssistantChange], args: argparse.Namespace) -> list[dict[str, Any]]:
    by_index: dict[int, dict[str, Any]] = {}
    order: list[int] = []

    for change in changes:
        entry = by_index.get(change.index)
        if entry is None:
            order.append(change.index)
            entry = {
                "index": change.index,
                "name": change.name,
                "first_old": change.old,
                "first_old_display": change.old_display,
                "final_new": change.new,
                "final_new_display": change.new_display,
                "first_elapsed": change.elapsed,
                "last_elapsed": change.elapsed,
                "change_count": 0,
            }
            by_index[change.index] = entry
        entry["final_new"] = change.new
        entry["final_new_display"] = change.new_display
        entry["last_elapsed"] = change.elapsed
        entry["change_count"] += 1

    details: list[dict[str, Any]] = []
    parameters: list[dict[str, Any]] = []
    for index in order:
        entry = by_index[index]
        target = entry["first_old"] if args.restore else synthesize_value(entry["first_old"], entry["final_new"], args.blend)
        detail = {
            **entry,
            "value": target,
            "mode": "restore" if args.restore else "blend",
        }
        details.append(detail)
        parameters.append({"index": index, "value": target})

    return [
        {
            "burst_id": 1,
            "elapsed": details[0]["last_elapsed"] if details else 0.0,
            "parameters": parameters,
            "details": details,
        }
    ] if parameters else []


def build_burst_batches(changes: list[AssistantChange], args: argparse.Namespace) -> list[dict[str, Any]]:
    if args.restore:
        raise RuntimeError("--restore is only supported with --mode final")

    batches_by_id: dict[int, dict[str, Any]] = {}
    order: list[int] = []

    for change in changes:
        batch = batches_by_id.get(change.burst_id)
        if batch is None:
            order.append(change.burst_id)
            batch = {"burst_id": change.burst_id, "elapsed": change.elapsed, "parameters": [], "details": []}
            batches_by_id[change.burst_id] = batch

        value = synthesize_value(change.old, change.new, args.blend)
        batch["parameters"].append({"index": change.index, "value": value})
        batch["details"].append(
            {
                "sequence": change.sequence,
                "index": change.index,
                "name": change.name,
                "old": change.old,
                "new": change.new,
                "old_display": change.old_display,
                "new_display": change.new_display,
                "value": value,
            }
        )

    return [batches_by_id[burst_id] for burst_id in order if batches_by_id[burst_id]["parameters"]]


def expected_final_values(batches: list[dict[str, Any]]) -> dict[int, float]:
    expected: dict[int, float] = {}
    for batch in batches:
        for parameter in batch.get("parameters", []):
            expected[int(parameter["index"])] = float(parameter["value"])
    return expected


def verify_values(current_parameters: Any, expected: dict[int, float], epsilon: float) -> list[dict[str, Any]]:
    if not isinstance(current_parameters, list):
        raise RuntimeError(f"list_parameters returned unexpected payload: {current_parameters!r}")

    current_by_index = {int(p["index"]): p for p in current_parameters if isinstance(p, dict) and "index" in p}
    mismatches: list[dict[str, Any]] = []

    for index, expected_value in expected.items():
        current = current_by_index.get(index)
        if current is None:
            mismatches.append({"index": index, "error": "missing_after_apply", "expected": expected_value})
            continue
        actual = finite_float(current.get("value"), "actual")
        if abs(actual - expected_value) > epsilon:
            mismatches.append(
                {
                    "index": index,
                    "name": current.get("name", ""),
                    "expected": expected_value,
                    "actual": actual,
                    "displayValue": current.get("displayValue", ""),
                }
            )

    return mismatches


def wait_for_verification(client: JsonRpcClient, expected: dict[int, float], args: argparse.Namespace) -> list[dict[str, Any]]:
    deadline = time.monotonic() + args.verify_timeout
    last_mismatches: list[dict[str, Any]] = []

    while True:
        current = client.tool("list_parameters")
        last_mismatches = verify_values(current, expected, args.verify_epsilon)
        if not last_mismatches:
            return []
        if time.monotonic() >= deadline:
            return last_mismatches
        time.sleep(max(0.01, args.verify_interval))


def apply_batches(args: argparse.Namespace, batches: list[dict[str, Any]]) -> list[dict[str, Any]]:
    if not args.token:
        raise RuntimeError("--token is required with --apply")

    results: list[dict[str, Any]] = []
    previous_elapsed: float | None = None

    with JsonRpcClient(args.host, args.port, args.token, args.timeout) as client:
        plugin_info = client.tool("get_plugin_info")
        results.append({"kind": "plugin_info", "response": plugin_info})

        for batch in batches:
            if args.mode == "burst" and args.preserve_timing and previous_elapsed is not None:
                delta = max(0.0, float(batch["elapsed"]) - previous_elapsed) * args.time_scale
                if args.max_delay is not None:
                    delta = min(delta, args.max_delay)
                delta = max(args.min_delay, delta)
                if delta > 0.0:
                    time.sleep(delta)

            response = client.tool("set_parameters_batch", {"parameters": batch["parameters"]})
            results.append(
                {
                    "kind": "set_parameters_batch",
                    "burst_id": batch["burst_id"],
                    "elapsed": batch["elapsed"],
                    "parameter_count": len(batch["parameters"]),
                    "response": response,
                }
            )
            previous_elapsed = float(batch["elapsed"])

        if args.verify:
            mismatches = wait_for_verification(client, expected_final_values(batches), args)
            results.append({"kind": "verify", "mismatches": mismatches, "mismatch_count": len(mismatches)})

    return results


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def print_plan_summary(plan: dict[str, Any], batches: list[dict[str, Any]], apply: bool) -> None:
    mode = "APPLY" if apply else "DRY-RUN"
    parameter_count = sum(len(batch["parameters"]) for batch in batches)
    print(f"{mode} {plan['mode']} plan: {len(batches)} batch(es), {parameter_count} parameter write(s)")
    print(f"source changes: {plan['source_change_count']} captured, {plan['filtered_change_count']} after filters")
    if plan.get("restore"):
        print("target: restore first observed pre-Assistant values")
    else:
        print(f"target: Assistant values with blend={plan['blend']}")

    preview: list[dict[str, Any]] = []
    for batch in batches:
        for detail in batch.get("details", []):
            preview.append(
                {
                    "index": detail["index"],
                    "name": detail["name"],
                    "value": detail["value"],
                    "from": detail.get("first_old", detail.get("old")),
                    "to": detail.get("final_new", detail.get("new")),
                }
            )
            if len(preview) >= 12:
                break
        if len(preview) >= 12:
            break
    print(json.dumps({"preview": preview}, indent=2, ensure_ascii=False))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--monitor", type=Path, default=DEFAULT_MONITOR, help="linked_instance_monitor JSONL capture.")
    parser.add_argument("--endpoint", default="ozone", help="Monitor endpoint name to read from. Use empty string for all.")
    parser.add_argument("--mode", choices=("final", "burst"), default="final")
    parser.add_argument("--restore", action="store_true", help="Apply first observed old values instead of Assistant values.")
    parser.add_argument("--blend", type=float, default=1.0, help="0=old value, 1=Assistant value, 0.5=half-strength.")
    parser.add_argument("--only-index", action="append", type=int, help="Only include this parameter index. Repeatable.")
    parser.add_argument("--exclude-index", action="append", type=int, help="Exclude this parameter index. Repeatable.")
    parser.add_argument("--name-contains", action="append", help="Only include parameters whose name contains this text.")
    parser.add_argument("--epsilon", type=float, default=DEFAULT_EPSILON)
    parser.add_argument("--out", type=Path, help="Write replay plan JSON here.")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT_DIR)

    parser.add_argument("--apply", action="store_true", help="Actually write through More-Phi MCP. Default is dry-run.")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--token", help="More-Phi MCP bearer token. Required with --apply.")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--verify", action="store_true", help="Read parameters after apply and compare expected final values.")
    parser.add_argument("--verify-epsilon", type=float, default=1.0e-5)
    parser.add_argument("--verify-timeout", type=float, default=2.0, help="Seconds to wait for queued parameter writes to settle.")
    parser.add_argument("--verify-interval", type=float, default=0.1, help="Polling interval while waiting for verification.")

    parser.add_argument("--preserve-timing", action="store_true", help="In burst mode, sleep according to captured burst spacing.")
    parser.add_argument("--time-scale", type=float, default=1.0, help="Scale captured timing when --preserve-timing is used.")
    parser.add_argument("--min-delay", type=float, default=0.0)
    parser.add_argument("--max-delay", type=float, help="Maximum delay between burst writes.")
    return parser


def main(argv: list[str]) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    if args.blend < 0.0 or args.blend > 1.0:
        parser.error("--blend must be between 0.0 and 1.0")
    if args.time_scale < 0.0:
        parser.error("--time-scale must be non-negative")

    endpoint = args.endpoint if args.endpoint else None
    source_changes = load_monitor_changes(args.monitor, endpoint, args.epsilon)
    filtered_changes = filter_changes(source_changes, args)
    if args.mode == "final":
        batches = build_final_batches(filtered_changes, args)
    else:
        batches = build_burst_batches(filtered_changes, args)

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out_path = args.out or (args.out_dir / f"ozone_assistant_replay_plan_{stamp}.json")
    plan: dict[str, Any] = {
        "schema_version": 1,
        "source": "more_phi_mcp_parameter_replay_plan",
        "safety": "No private iZotope IPC is synthesized or injected; writes use More-Phi MCP set_parameters_batch only.",
        "generated_at_unix": time.time(),
        "monitor": str(args.monitor),
        "endpoint": endpoint,
        "mode": args.mode,
        "restore": bool(args.restore),
        "blend": args.blend,
        "source_change_count": len(source_changes),
        "filtered_change_count": len(filtered_changes),
        "batch_count": len(batches),
        "parameter_write_count": sum(len(batch["parameters"]) for batch in batches),
        "batches": batches,
    }

    print_plan_summary(plan, batches, args.apply)

    if args.apply:
        plan["apply"] = {
            "host": args.host,
            "port": args.port,
            "verify": bool(args.verify),
            "results": apply_batches(args, batches),
        }
    else:
        print("dry-run only; add --apply --token <token> to write through More-Phi MCP")

    write_json(out_path, plan)
    print(f"plan written -> {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
