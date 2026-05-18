#!/usr/bin/env python3
"""
Summarize iZotope IPC decode JSONL captures.

This extracts:
  - event counts
  - payload format tags
  - socket/HTTP endpoints
  - file and mapping names
  - native backtrace frames grouped by module-relative RVA

Use after tools/izotope_ipc_decode.py.
"""

from __future__ import annotations

import argparse
import json
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


def load_events(path: Path) -> list[dict[str, Any]]:
    events = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        try:
            events.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return events


def parse_addr(value: str | None) -> int | None:
    if not value:
        return None
    try:
        return int(value, 16)
    except ValueError:
        return None


def module_ranges(events: list[dict[str, Any]]) -> list[tuple[int, int, str, str]]:
    ranges = []
    for ev in events:
        if ev.get("kind") != "module":
            continue
        base = parse_addr(ev.get("base"))
        size = ev.get("size")
        if base is None or not isinstance(size, int):
            continue
        ranges.append((base, base + size, ev.get("name", ""), ev.get("path", "")))
    ranges.sort()
    return ranges


def module_for_addr(ranges: list[tuple[int, int, str, str]], addr: int) -> tuple[str, str, int] | None:
    for start, end, name, path in ranges:
        if start <= addr < end:
            return name, path, addr - start
    return None


def interesting_module(name: str, path: str) -> bool:
    text = f"{name} {path}".lower()
    return any(s in text for s in ("izotope", "ozone", "neutron", "relay", "tonal", "morephi"))


def summarize(path: Path) -> dict[str, Any]:
    events = load_events(path)
    ranges = module_ranges(events)
    counts = Counter(ev.get("kind", "") for ev in events)
    payload_tags = Counter()
    names = Counter()
    endpoints = Counter()
    file_payloads = []
    interesting_frames = Counter()
    stack_samples: dict[str, dict[str, Any]] = {}

    for ev in events:
        for tag in ev.get("payload_tags", []) or []:
            payload_tags[tag] += 1
        if ev.get("name"):
            names[str(ev["name"])] += 1
        endpoint = ev.get("endpoint")
        if isinstance(endpoint, dict):
            endpoint = endpoint.get("endpoint")
        if endpoint:
            endpoints[str(endpoint)] += 1
        info = ev.get("info")
        if isinstance(info, dict):
            if info.get("server") or info.get("url"):
                endpoints[str(info)] += 1
        if ev.get("payload_len") and ev.get("name"):
            file_payloads.append(
                {
                    "kind": ev.get("kind"),
                    "name": ev.get("name"),
                    "length": ev.get("length"),
                    "payload_len": ev.get("payload_len"),
                    "tags": ev.get("payload_tags", []),
                    "ascii": ev.get("payload_ascii_preview", "")[:140],
                }
            )

        for frame in ev.get("backtrace", []) or []:
            addr = parse_addr(frame.get("address"))
            if addr is None:
                continue
            mod = module_for_addr(ranges, addr)
            if not mod:
                continue
            name, module_path, rva = mod
            if not interesting_module(name, module_path):
                continue
            key = f"{name}+0x{rva:X}"
            interesting_frames[key] += 1
            stack_samples.setdefault(
                key,
                {
                    "module": name,
                    "path": module_path,
                    "rva": f"0x{rva:X}",
                    "event_kind": ev.get("kind"),
                    "event_name": ev.get("name"),
                    "symbol": frame.get("symbol"),
                },
            )

    return {
        "path": str(path),
        "event_counts": dict(counts.most_common()),
        "payload_tags": dict(payload_tags.most_common()),
        "names": dict(names.most_common(40)),
        "endpoints": dict(endpoints.most_common(40)),
        "file_payloads": file_payloads[:30],
        "interesting_backtrace_frames": [
            {**stack_samples[key], "count": count}
            for key, count in interesting_frames.most_common(80)
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("jsonl", nargs="?", help="Capture JSONL. Defaults to latest ipc_decode JSONL.")
    parser.add_argument("--out", help="Optional summary JSON path.")
    args = parser.parse_args()

    if args.jsonl:
        path = Path(args.jsonl)
    else:
        candidates = sorted(
            Path("tools/live_captures/ipc_decode").glob("izotope_ipc_decode_*.jsonl"),
            key=lambda p: p.stat().st_mtime,
            reverse=True,
        )
        if not candidates:
            raise SystemExit("No capture JSONL found.")
        path = candidates[0]

    summary = summarize(path)
    text = json.dumps(summary, indent=2)
    print(text)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
