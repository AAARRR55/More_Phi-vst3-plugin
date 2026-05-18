#!/usr/bin/env python3
"""Decode candidate Ozone Assistant arg2 work-object fields from trace artifacts.

This is an offline artifact decoder. It reads JSONL files produced by
izotope_state_diff_trace.py and linked_instance_monitor.py. It does not attach
to FL Studio, read process memory, write process memory, or call any plugin.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from collections import Counter, defaultdict
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_TRACE = Path("tools/live_captures/ipc_decode/izotope_state_diff_trace_20260516_043615.jsonl")
DEFAULT_MONITOR = Path("tools/live_captures/linked_instances/linked_instance_monitor_20260516_043614.jsonl")


@dataclass(frozen=True)
class ParamChange:
    elapsed: float
    wall_ms: int
    index: int
    name: str
    old: float
    new: float
    old_display: str
    new_display: str


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    with path.open("r", encoding="utf-8") as fp:
        for line_no, line in enumerate(fp, 1):
            line = line.strip()
            if not line:
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(f"{path}:{line_no}: invalid JSON: {exc}") from exc
            if isinstance(item, dict):
                events.append(item)
    return events


def decode_hex_bytes(text: str | None) -> bytes:
    if not text:
        return b""
    cleaned = text.replace(" ", "")
    if len(cleaned) % 2 != 0:
        return b""
    try:
        return bytes.fromhex(cleaned)
    except ValueError:
        return b""


def scalar_decodes(raw: bytes) -> dict[str, Any]:
    out: dict[str, Any] = {"hex": raw.hex(" ")}
    if len(raw) >= 1:
        out["u8"] = raw[0]
        out["i8"] = struct.unpack("<b", raw[:1])[0]
    if len(raw) >= 2:
        out["u16le"] = struct.unpack("<H", raw[:2])[0]
        out["i16le"] = struct.unpack("<h", raw[:2])[0]
    if len(raw) >= 4:
        u32 = struct.unpack("<I", raw[:4])[0]
        i32 = struct.unpack("<i", raw[:4])[0]
        f32 = struct.unpack("<f", raw[:4])[0]
        out["u32le"] = u32
        out["i32le"] = i32
        if math.isfinite(f32):
            out["f32le"] = f32
    if len(raw) >= 8:
        u64 = struct.unpack("<Q", raw[:8])[0]
        f64 = struct.unpack("<d", raw[:8])[0]
        out["u64le"] = u64
        if math.isfinite(f64):
            out["f64le"] = f64
    return out


def load_parameter_changes(path: Path) -> tuple[list[ParamChange], list[dict[str, Any]]]:
    events = load_jsonl(path)
    changes: list[ParamChange] = []
    bursts: list[dict[str, Any]] = []
    for event in events:
        raw_changes = event.get("changes")
        if not isinstance(raw_changes, list) or not raw_changes:
            continue
        wall_ms = int(float(event.get("t_wall", 0.0)) * 1000.0)
        elapsed = float(event.get("elapsed", 0.0))
        burst_changes: list[ParamChange] = []
        for raw in raw_changes:
            try:
                change = ParamChange(
                    elapsed=elapsed,
                    wall_ms=wall_ms,
                    index=int(raw.get("index")),
                    name=str(raw.get("name", "")),
                    old=float(raw.get("old", 0.0)),
                    new=float(raw.get("new", 0.0)),
                    old_display=str(raw.get("oldDisplay", "")),
                    new_display=str(raw.get("newDisplay", "")),
                )
            except (TypeError, ValueError):
                continue
            changes.append(change)
            burst_changes.append(change)
        bursts.append(
            {
                "elapsed": elapsed,
                "wall_ms": wall_ms,
                "change_count": len(burst_changes),
                "first_param": burst_changes[0].name if burst_changes else "",
                "changes": burst_changes,
            }
        )
    return changes, bursts


def read_snapshot(path_text: str | None, repo_root: Path) -> bytes:
    if not path_text:
        return b""
    path = Path(path_text)
    if not path.is_absolute():
        path = repo_root / path
    try:
        return path.read_bytes()
    except OSError:
        return b""


def pack_param_index(index: int) -> dict[str, bytes]:
    return {
        "u16le": struct.pack("<H", index & 0xFFFF),
        "u32le": struct.pack("<I", index & 0xFFFFFFFF),
    }


def pack_f32(value: float) -> bytes:
    return struct.pack("<f", float(value))


def find_all(buf: bytes, needle: bytes, limit: int = 64) -> list[int]:
    if not buf or not needle:
        return []
    positions: list[int] = []
    start = 0
    while len(positions) < limit:
        pos = buf.find(needle, start)
        if pos < 0:
            break
        positions.append(pos)
        start = pos + 1
    return positions


def is_near_ranges(offset: int, ranges: list[dict[str, Any]], margin: int) -> bool:
    for item in ranges:
        try:
            start = int(item.get("start", 0))
            end = int(item.get("end", start + int(item.get("len", 0))))
        except (TypeError, ValueError):
            continue
        if start - margin <= offset <= end + margin:
            return True
    return False


def find_approx_f32(buf: bytes, value: float, epsilon: float, limit: int = 64) -> list[int]:
    if len(buf) < 4:
        return []
    positions: list[int] = []
    # Restrict to aligned float slots. Unaligned scans produce many accidental
    # matches in pointer-heavy C++ objects and are not useful for this decoder.
    for offset in range(0, len(buf) - 3, 4):
        raw = buf[offset : offset + 4]
        candidate = struct.unpack("<f", raw)[0]
        if not math.isfinite(candidate):
            continue
        if abs(candidate - value) <= epsilon:
            positions.append(offset)
            if len(positions) >= limit:
                break
    return positions


def low_information_float(value: float, epsilon: float) -> bool:
    """Return true for common values that create too many false positives."""
    common = (0.0, 0.5, 1.0)
    return any(abs(value - item) <= epsilon for item in common)


def summarize_range_decodes(event: dict[str, Any]) -> list[dict[str, Any]]:
    out: list[dict[str, Any]] = []
    ranges = event.get("ranges")
    if not isinstance(ranges, list):
        return out
    for item in ranges:
        if not isinstance(item, dict):
            continue
        before = decode_hex_bytes(item.get("before_hex"))
        after = decode_hex_bytes(item.get("after_hex"))
        rec = {
            "start": item.get("start"),
            "end": item.get("end"),
            "len": item.get("len"),
            "before": scalar_decodes(before),
            "after": scalar_decodes(after),
        }
        out.append(rec)
    return out


def module_for_param(name: str) -> str:
    if ":" in name:
        return name.split(":", 1)[0].strip()
    return name.split(" ", 1)[0].strip()


def build_decoder_report(
    trace_path: Path,
    monitor_path: Path,
    repo_root: Path,
    window_ms: int,
    epsilon: float,
    high_value_min_bytes: int,
    near_margin: int,
    include_common_floats: bool,
) -> dict[str, Any]:
    trace_events = load_jsonl(trace_path)
    param_changes, bursts = load_parameter_changes(monitor_path)

    ready_event = next((e for e in trace_events if e.get("kind") == "trace_ready"), None)
    ready_ms = int(ready_event.get("ts_ms", 0)) if ready_event else 0
    state_changes = [e for e in trace_events if e.get("kind") == "state_changed"]

    event_counts = Counter(str(e.get("kind")) for e in trace_events)
    changes_by_arg = Counter(str(e.get("arg_index")) for e in state_changes)
    changes_by_pointer = Counter(str(e.get("pointer")) for e in state_changes)
    changed_bytes_by_arg: defaultdict[str, int] = defaultdict(int)
    for e in state_changes:
        changed_bytes_by_arg[str(e.get("arg_index"))] += int(e.get("changed_bytes") or 0)

    offset_frequency: Counter[tuple[str, int, int]] = Counter()
    byte_transition_frequency: Counter[tuple[int, str, str]] = Counter()
    for e in state_changes:
        arg = str(e.get("arg_index"))
        for item in e.get("ranges") or []:
            if not isinstance(item, dict):
                continue
            try:
                start = int(item.get("start"))
                length = int(item.get("len"))
            except (TypeError, ValueError):
                continue
            offset_frequency[(arg, start, length)] += 1
            byte_transition_frequency[(length, str(item.get("before_hex")), str(item.get("after_hex")))] += 1

    high_value_events = [
        e
        for e in state_changes
        if int(e.get("arg_index") or -1) == 2 and int(e.get("changed_bytes") or 0) >= high_value_min_bytes
    ]

    burst_correlations: list[dict[str, Any]] = []
    all_match_records: list[dict[str, Any]] = []
    for burst in bursts:
        wall_ms = int(burst["wall_ms"])
        nearby = [
            e for e in state_changes if abs(int(e.get("ts_ms", 0)) - wall_ms) <= window_ms
        ]
        nearby.sort(key=lambda e: (abs(int(e.get("ts_ms", 0)) - wall_ms), int(e.get("call_index", 0))))

        burst_record: dict[str, Any] = {
            "elapsed": burst["elapsed"],
            "wall_ms": wall_ms,
            "change_count": burst["change_count"],
            "first_param": burst["first_param"],
            "param_indices": [c.index for c in burst["changes"]],
            "nearby_trace_events": [],
        }

        for event in nearby[:12]:
            snapshot = read_snapshot(event.get("snapshot_file"), repo_root)
            ranges = event.get("ranges") if isinstance(event.get("ranges"), list) else []
            param_hits: list[dict[str, Any]] = []
            for change in burst["changes"]:
                index_hits: dict[str, list[int]] = {}
                for kind, needle in pack_param_index(change.index).items():
                    positions = find_all(snapshot, needle, limit=16)
                    near_positions = [p for p in positions if is_near_ranges(p, ranges, near_margin)]
                    if near_positions:
                        index_hits[f"{kind}_near"] = near_positions[:8]

                value_hits: dict[str, list[int]] = {}
                for label, value in (("old", change.old), ("new", change.new)):
                    if (not include_common_floats) and low_information_float(value, epsilon):
                        continue
                    approx_positions = find_approx_f32(snapshot, value, epsilon, limit=32)
                    near_approx = [p for p in approx_positions if is_near_ranges(p, ranges, near_margin)]
                    if near_approx:
                        value_hits[f"{label}_f32_aligned_near"] = near_approx[:8]

                if index_hits or value_hits:
                    hit = {
                        "index": change.index,
                        "name": change.name,
                        "old": change.old,
                        "new": change.new,
                        "index_hits": index_hits,
                        "value_hits": value_hits,
                    }
                    param_hits.append(hit)
                    all_match_records.append(
                        {
                            "burst_elapsed": burst["elapsed"],
                            "call_index": event.get("call_index"),
                            "arg_index": event.get("arg_index"),
                            "pointer": event.get("pointer"),
                            "changed_bytes": event.get("changed_bytes"),
                            "param": hit,
                        }
                    )

            burst_record["nearby_trace_events"].append(
                {
                    "call_index": event.get("call_index"),
                    "arg_index": event.get("arg_index"),
                    "pointer": event.get("pointer"),
                    "changed_bytes": event.get("changed_bytes"),
                    "delta_ms": int(event.get("ts_ms", 0)) - wall_ms,
                    "ranges": summarize_range_decodes(event)[:8],
                    "param_hits": param_hits[:10],
                }
            )
        burst_correlations.append(burst_record)

    module_counts = Counter(module_for_param(c.name) for c in param_changes)
    unique_param_changes = []
    for change in param_changes:
        unique_param_changes.append(
            {
                "elapsed": change.elapsed,
                "index": change.index,
                "name": change.name,
                "old": change.old,
                "new": change.new,
                "oldDisplay": change.old_display,
                "newDisplay": change.new_display,
            }
        )

    return {
        "inputs": {
            "trace_path": str(trace_path),
            "monitor_path": str(monitor_path),
            "window_ms": window_ms,
            "float_epsilon": epsilon,
            "high_value_min_bytes": high_value_min_bytes,
            "near_margin": near_margin,
            "include_common_floats": include_common_floats,
        },
        "trace_summary": {
            "event_counts": dict(event_counts),
            "state_changed_count": len(state_changes),
            "changes_by_arg": dict(changes_by_arg),
            "changed_bytes_by_arg": dict(changed_bytes_by_arg),
            "changes_by_pointer": dict(changes_by_pointer),
            "ready_ts_ms": ready_ms,
        },
        "parameter_summary": {
            "change_count": len(param_changes),
            "burst_count": len(bursts),
            "module_counts": dict(module_counts),
            "changes": unique_param_changes,
        },
        "top_offsets": [
            {"arg": arg, "start": start, "len": length, "count": count}
            for (arg, start, length), count in offset_frequency.most_common(80)
        ],
        "top_byte_transitions": [
            {"len": length, "before_hex": before, "after_hex": after, "count": count}
            for (length, before, after), count in byte_transition_frequency.most_common(80)
        ],
        "high_value_events": [
            {
                "call_index": e.get("call_index"),
                "arg_index": e.get("arg_index"),
                "pointer": e.get("pointer"),
                "changed_bytes": e.get("changed_bytes"),
                "elapsed_from_ready_ms": (int(e.get("ts_ms", 0)) - ready_ms) if ready_ms else None,
                "ranges": summarize_range_decodes(e)[:16],
                "snapshot_file": e.get("snapshot_file"),
            }
            for e in high_value_events[:200]
        ],
        "burst_correlations": burst_correlations,
        "param_match_records": all_match_records[:500],
    }


def write_markdown(report: dict[str, Any], path: Path) -> None:
    trace = report["trace_summary"]
    params = report["parameter_summary"]
    lines: list[str] = []
    lines.append("# Ozone Assistant Arg2 Decode Report")
    lines.append("")
    lines.append(f"Generated: `{datetime.now().isoformat(timespec='seconds')}`")
    lines.append("")
    lines.append("## Inputs")
    lines.append("")
    for key, value in report["inputs"].items():
        lines.append(f"- `{key}`: `{value}`")
    lines.append("")
    lines.append("## Summary")
    lines.append("")
    lines.append(f"- State-change events: `{trace['state_changed_count']}`")
    lines.append(f"- Changes by arg: `{trace['changes_by_arg']}`")
    lines.append(f"- Changed bytes by arg: `{trace['changed_bytes_by_arg']}`")
    lines.append(f"- Parameter changes: `{params['change_count']}` across `{params['burst_count']}` bursts")
    lines.append(f"- Parameter modules: `{params['module_counts']}`")
    lines.append("")
    lines.append("## Parameter Bursts")
    lines.append("")
    lines.append("| Elapsed | Changes | First Parameter | Nearby Trace Events |")
    lines.append("| ---: | ---: | --- | ---: |")
    for burst in report["burst_correlations"]:
        lines.append(
            f"| `{burst['elapsed']:.2f}s` | `{burst['change_count']}` | "
            f"`{burst['first_param']}` | `{len(burst['nearby_trace_events'])}` |"
        )
    lines.append("")
    lines.append("## Top Offsets")
    lines.append("")
    lines.append("| Arg | Offset | Len | Count |")
    lines.append("| ---: | ---: | ---: | ---: |")
    for item in report["top_offsets"][:30]:
        lines.append(f"| `{item['arg']}` | `{item['start']}` | `{item['len']}` | `{item['count']}` |")
    lines.append("")
    lines.append("## Top Byte Transitions")
    lines.append("")
    lines.append("| Len | Before | After | Count |")
    lines.append("| ---: | --- | --- | ---: |")
    for item in report["top_byte_transitions"][:25]:
        lines.append(
            f"| `{item['len']}` | `{item['before_hex']}` | `{item['after_hex']}` | `{item['count']}` |"
        )
    lines.append("")
    lines.append("## Candidate Param Matches")
    lines.append("")
    if not report["param_match_records"]:
        lines.append("No exact or approximate parameter index/value matches were found near changed byte ranges.")
    else:
        lines.append("| Burst | Call | Arg | Ptr | Param | Hits |")
        lines.append("| ---: | ---: | ---: | --- | --- | --- |")
        for item in report["param_match_records"][:60]:
            param = item["param"]
            hit_keys = list(param.get("index_hits", {}).keys()) + list(param.get("value_hits", {}).keys())
            lines.append(
                f"| `{item['burst_elapsed']:.2f}s` | `{item['call_index']}` | `{item['arg_index']}` | "
                f"`{item['pointer']}` | `{param['index']} {param['name']}` | `{', '.join(hit_keys)}` |"
            )
    lines.append("")
    lines.append("## High-Value Events")
    lines.append("")
    lines.append("| Call | Arg | Ptr | Changed Bytes | Elapsed From Trace Ready | First Ranges |")
    lines.append("| ---: | ---: | --- | ---: | ---: | --- |")
    for event in report["high_value_events"][:40]:
        ranges = []
        for item in event.get("ranges", [])[:4]:
            before = item.get("before", {}).get("hex", "")
            after = item.get("after", {}).get("hex", "")
            ranges.append(f"{item.get('start')}:{item.get('len')} {before}->{after}")
        elapsed_ms = event.get("elapsed_from_ready_ms")
        elapsed = "" if elapsed_ms is None else f"{elapsed_ms / 1000.0:.2f}s"
        lines.append(
            f"| `{event['call_index']}` | `{event['arg_index']}` | `{event['pointer']}` | "
            f"`{event['changed_bytes']}` | `{elapsed}` | `{'; '.join(ranges)}` |"
        )
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--trace", type=Path, default=DEFAULT_TRACE)
    parser.add_argument("--monitor", type=Path, default=DEFAULT_MONITOR)
    parser.add_argument("--out-dir", type=Path, default=Path("tools/live_captures/ipc_decode"))
    parser.add_argument("--window-ms", type=int, default=3000)
    parser.add_argument("--float-epsilon", type=float, default=1.0e-6)
    parser.add_argument("--near-margin", type=int, default=128)
    parser.add_argument("--high-value-min-bytes", type=int, default=16)
    parser.add_argument(
        "--include-common-floats",
        action="store_true",
        help="Also match common 0.0/0.5/1.0 float values. Disabled by default to reduce false positives.",
    )
    args = parser.parse_args()

    repo_root = Path.cwd()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    report = build_decoder_report(
        trace_path=args.trace,
        monitor_path=args.monitor,
        repo_root=repo_root,
        window_ms=args.window_ms,
        epsilon=args.float_epsilon,
        high_value_min_bytes=args.high_value_min_bytes,
        near_margin=args.near_margin,
        include_common_floats=args.include_common_floats,
    )

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    json_path = args.out_dir / f"ozone_arg2_decode_{stamp}.json"
    md_path = args.out_dir / f"ozone_arg2_decode_{stamp}.md"
    json_path.write_text(json.dumps(report, indent=2), encoding="utf-8")
    write_markdown(report, md_path)

    print(f"Wrote JSON report: {json_path}")
    print(f"Wrote markdown report: {md_path}")
    print(
        "Summary: "
        f"{report['trace_summary']['state_changed_count']} state changes, "
        f"{report['parameter_summary']['change_count']} parameter changes, "
        f"{len(report['param_match_records'])} near-range param matches."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
