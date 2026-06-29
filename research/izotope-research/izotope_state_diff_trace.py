#!/usr/bin/env python3
"""
Trace Ozone Assistant state-object changes from a known internal function.

This is read-only Frida instrumentation. It does not patch code or write into
the target process. The default hook is the Ozone Master Assistant state
function discovered by tools/izotope_data_watch.py:

  iZOzonePro.dll+0xEAD3E0

For each call, the script snapshots selected pointer arguments and emits only
the first snapshot for a pointer plus later byte-range diffs. Full changed
snapshots are saved as .bin files next to the JSONL trace so offsets can be
recovered with normal binary diff tooling.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import signal
import time
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_TARGET = "iZOzonePro.dll+0xEAD3E0:ozone_master_assistant_state"


JS_TEMPLATE = r"""
'use strict';

const TARGET = __TARGET__;
const SNAPSHOT_ARGS = __SNAPSHOT_ARGS__;
const SNAPSHOT_BYTES = __SNAPSHOT_BYTES__;
const MAX_RANGES = __MAX_RANGES__;
const HEARTBEAT_EVERY = __HEARTBEAT_EVERY__;
const FOLLOW_POINTERS = __FOLLOW_POINTERS__;
const FOLLOW_BYTES = __FOLLOW_BYTES__;
const FOLLOW_STRIDE = __FOLLOW_STRIDE__;
const MAX_FOLLOW_POINTERS = __MAX_FOLLOW_POINTERS__;

const seen = {};
let callCount = 0;

function emit(kind, data, bytes) {
  data = data || {};
  data.kind = kind;
  data.ts_ms = Date.now();
  if (bytes) {
    send(data, bytes);
  } else {
    send(data);
  }
}

function moduleInfo(addr) {
  try {
    const m = Process.findModuleByAddress(addr);
    if (!m) return null;
    return {
      name: m.name,
      path: m.path,
      base: String(m.base),
      rva: '0x' + ptr(addr).sub(m.base).toString(16)
    };
  } catch (_) {
    return null;
  }
}

function safeAscii(u8, maxLen) {
  let out = '';
  const n = Math.min(u8.length, maxLen);
  for (let i = 0; i < n; ++i) {
    const b = u8[i];
    out += (b >= 32 && b < 127) ? String.fromCharCode(b) : '.';
  }
  return out;
}

function hexPreview(u8, maxLen) {
  const parts = [];
  const n = Math.min(u8.length, maxLen);
  for (let i = 0; i < n; ++i) {
    parts.push(('0' + u8[i].toString(16)).slice(-2));
  }
  return parts.join(' ');
}

function fnv1a32(u8) {
  let h = 0x811c9dc5;
  for (let i = 0; i < u8.length; ++i) {
    h ^= u8[i];
    h = Math.imul(h, 0x01000193) >>> 0;
  }
  return ('00000000' + h.toString(16)).slice(-8);
}

function readSnapshot(p) {
  if (p.isNull()) return null;
  const range = Process.findRangeByAddress(p);
  if (!range || range.protection.indexOf('r') < 0) return null;

  let available = SNAPSHOT_BYTES;
  try {
    const offset = p.sub(range.base).toUInt32();
    available = Math.max(0, Math.min(SNAPSHOT_BYTES, range.size - offset));
  } catch (_) {
    available = SNAPSHOT_BYTES;
  }
  if (available <= 0) return null;

  try {
    const raw = p.readByteArray(available);
    if (!raw) return null;
    return new Uint8Array(raw);
  } catch (_) {
    return null;
  }
}

function pointerFromBytes(u8, offset) {
  if (offset < 0 || offset + 8 > u8.length) return ptr(0);
  let value = 0n;
  for (let i = 7; i >= 0; --i) {
    value = (value << 8n) + BigInt(u8[offset + i]);
  }
  return ptr('0x' + value.toString(16));
}

function isReadablePointer(p) {
  if (p.isNull()) return false;
  try {
    const range = Process.findRangeByAddress(p);
    return !!range && range.protection.indexOf('r') >= 0;
  } catch (_) {
    return false;
  }
}

function diffRanges(prev, curr) {
  const ranges = [];
  const n = Math.min(prev.length, curr.length);
  let changed = 0;
  let i = 0;
  while (i < n) {
    if (prev[i] === curr[i]) {
      ++i;
      continue;
    }
    const start = i;
    while (i < n && prev[i] !== curr[i]) {
      ++i;
    }
    const end = i;
    changed += end - start;
    if (ranges.length < MAX_RANGES) {
      ranges.push({
        start: start,
        end: end,
        len: end - start,
        before_hex: hexPreview(prev.slice(start, Math.min(end, start + 32)), 32),
        after_hex: hexPreview(curr.slice(start, Math.min(end, start + 32)), 32),
        before_ascii: safeAscii(prev.slice(start, Math.min(end, start + 32)), 32),
        after_ascii: safeAscii(curr.slice(start, Math.min(end, start + 32)), 32)
      });
    }
  }
  if (prev.length !== curr.length) {
    changed += Math.abs(prev.length - curr.length);
    if (ranges.length < MAX_RANGES) {
      ranges.push({ start: n, end: Math.max(prev.length, curr.length), len: Math.abs(prev.length - curr.length), size_changed: true });
    }
  }
  return { changed_bytes: changed, ranges: ranges, truncated: ranges.length >= MAX_RANGES };
}

function describeArg(v) {
  const p = ptr(v);
  const rec = { value: String(p) };
  const snap = readSnapshot(p);
  if (snap) {
    rec.mem_len = snap.length;
    rec.mem_hash32 = fnv1a32(snap);
    rec.mem_hex = hexPreview(snap, 32);
    rec.mem_ascii = safeAscii(snap, 64);
  }
  return rec;
}

const modules = {};
Process.enumerateModules().forEach(function (m) {
  modules[m.name.toLowerCase()] = m;
  if (/izotope|ozone|neutron|relay|morephi/i.test(m.name + ' ' + m.path)) {
    emit('module', { name: m.name, path: m.path, base: String(m.base), size: m.size });
  }
});

const m = modules[TARGET.module.toLowerCase()];
if (!m) {
  emit('target_missing_module', TARGET);
} else {
  const addr = m.base.add(TARGET.rva);
  emit('target_hook', { module: m.name, rva: '0x' + TARGET.rva.toString(16), address: String(addr), label: TARGET.label });

  Interceptor.attach(addr, {
    onEnter(args) {
      callCount++;
      this.label = TARGET.label;
      this.call_index = callCount;
      this.return_address = this.returnAddress;

      const quickArgs = [];
      for (let i = 0; i < 6; ++i) quickArgs.push(describeArg(args[i]));

      if (HEARTBEAT_EVERY > 0 && (callCount % HEARTBEAT_EVERY) === 0) {
        emit('call_heartbeat', {
          label: TARGET.label,
          call_index: callCount,
          thread_id: Process.getCurrentThreadId(),
          return_address: String(this.returnAddress),
          return_module: moduleInfo(this.returnAddress),
          args: quickArgs
        });
      }

      for (const argIndex of SNAPSHOT_ARGS) {
        const p = ptr(args[argIndex]);
        const snap = readSnapshot(p);
        if (!snap) continue;

        processSnapshot('state', argIndex, -1, p, snap, quickArgs, this.returnAddress, callCount);

        if (FOLLOW_POINTERS) {
          let followed = 0;
          const limit = Math.min(snap.length, SNAPSHOT_BYTES);
          for (let off = 0; off + 8 <= limit && followed < MAX_FOLLOW_POINTERS; off += FOLLOW_STRIDE) {
            const child = pointerFromBytes(snap, off);
            if (!isReadablePointer(child)) continue;
            const childSnap = readSnapshot(child);
            if (!childSnap) continue;
            const trimmed = childSnap.slice(0, Math.min(FOLLOW_BYTES, childSnap.length));
            processSnapshot('child_state', argIndex, off, child, trimmed, quickArgs, this.returnAddress, callCount);
            followed++;
          }
        }
      }
    },
    onLeave(retval) {
      emit('call_leave', {
        label: this.label,
        call_index: this.call_index,
        retval: String(retval)
      });
    }
  });
}

emit('trace_ready', { target: TARGET, snapshot_args: SNAPSHOT_ARGS, snapshot_bytes: SNAPSHOT_BYTES });

function processSnapshot(kindPrefix, argIndex, parentOffset, p, snap, quickArgs, retAddr, callIndex) {
  const key = TARGET.label + ':' + kindPrefix + ':arg' + argIndex + ':off' + parentOffset + ':' + String(p);
  const hash = fnv1a32(snap);
  const previous = seen[key];
  const baseEvent = {
    label: TARGET.label,
    call_index: callIndex,
    arg_index: argIndex,
    parent_offset: parentOffset,
    pointer: String(p),
    len: snap.length,
    hash32: hash,
    thread_id: Process.getCurrentThreadId(),
    return_address: String(retAddr),
    return_module: moduleInfo(retAddr),
    ascii_preview: safeAscii(snap, 128),
    hex_preview: hexPreview(snap, 64),
    args: quickArgs
  };

  if (!previous) {
    seen[key] = snap.slice(0);
    emit(kindPrefix + '_initial', baseEvent, snap.buffer);
    return;
  }

  if (previous.length !== snap.length || fnv1a32(previous) !== hash) {
    const d = diffRanges(previous, snap);
    seen[key] = snap.slice(0);
    baseEvent.changed_bytes = d.changed_bytes;
    baseEvent.ranges = d.ranges;
    baseEvent.ranges_truncated = d.truncated;
    emit(kindPrefix + '_changed', baseEvent, snap.buffer);
  }
}
"""


def parse_target(raw: str) -> dict[str, Any]:
    label = ""
    spec = raw
    if ":" in raw:
        spec, label = raw.split(":", 1)
    module, rva_text = spec.split("+", 1)
    return {"module": module, "rva": int(rva_text, 16), "label": label or spec}


def make_js(
    target: dict[str, Any],
    snapshot_args: list[int],
    snapshot_bytes: int,
    max_ranges: int,
    heartbeat_every: int,
    follow_pointers: bool,
    follow_bytes: int,
    follow_stride: int,
    max_follow_pointers: int,
) -> str:
    return (
        JS_TEMPLATE
        .replace("__TARGET__", json.dumps(target, separators=(",", ":")))
        .replace("__SNAPSHOT_ARGS__", json.dumps(snapshot_args, separators=(",", ":")))
        .replace("__SNAPSHOT_BYTES__", str(snapshot_bytes))
        .replace("__MAX_RANGES__", str(max_ranges))
        .replace("__HEARTBEAT_EVERY__", str(heartbeat_every))
        .replace("__FOLLOW_POINTERS__", "true" if follow_pointers else "false")
        .replace("__FOLLOW_BYTES__", str(follow_bytes))
        .replace("__FOLLOW_STRIDE__", str(follow_stride))
        .replace("__MAX_FOLLOW_POINTERS__", str(max_follow_pointers))
    )


def safe_filename(text: str) -> str:
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", text).strip("_")[:120]


def ascii_preview(data: bytes | None, limit: int = 160) -> str:
    if not data:
        return ""
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data[:limit])


def hex_preview(data: bytes | None, limit: int = 64) -> str:
    if not data:
        return ""
    return " ".join(f"{b:02x}" for b in data[:limit])


def summarize(path: Path) -> dict[str, Any]:
    counts = Counter()
    changed_by_arg = Counter()
    changed_ranges: list[dict[str, Any]] = []
    returns = Counter()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        ev = json.loads(line)
        kind = ev.get("kind", "")
        counts[kind] += 1
        if kind in {"state_changed", "child_state_changed"}:
            key = f"arg{ev.get('arg_index')}"
            if kind == "child_state_changed":
                key += f"+child@0x{int(ev.get('parent_offset', 0)):X}"
            changed_by_arg[key] += 1
            for r in ev.get("ranges", [])[:8]:
                changed_ranges.append(
                    {
                        "kind": kind,
                        "arg": ev.get("arg_index"),
                        "parent_offset": ev.get("parent_offset"),
                        "pointer": ev.get("pointer"),
                        "call_index": ev.get("call_index"),
                        "start": r.get("start"),
                        "len": r.get("len"),
                        "before_hex": r.get("before_hex"),
                        "after_hex": r.get("after_hex"),
                    }
                )
        elif kind == "call_leave":
            returns[ev.get("retval", "")] += 1
    return {
        "event_counts": dict(counts.most_common()),
        "changes_by_arg": dict(changed_by_arg.most_common()),
        "return_values": dict(returns.most_common()),
        "sample_changed_ranges": changed_ranges[:40],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, help="Target PID. Defaults to first FL64.exe.")
    parser.add_argument("--target", default=DEFAULT_TARGET, help="module.dll+0xRVA[:label]")
    parser.add_argument("--duration", type=float, default=45.0)
    parser.add_argument("--snapshot-args", default="0,1", help="Comma-separated argument indices to snapshot.")
    parser.add_argument("--snapshot-bytes", type=int, default=2048)
    parser.add_argument("--max-ranges", type=int, default=32)
    parser.add_argument("--heartbeat-every", type=int, default=100, help="Emit call metadata every N calls; 0 disables.")
    parser.add_argument("--follow-pointers", action="store_true", help="Also snapshot readable pointer fields inside each selected argument.")
    parser.add_argument("--follow-bytes", type=int, default=512, help="Bytes to snapshot for each followed child pointer.")
    parser.add_argument("--follow-stride", type=int, default=8, help="Pointer scan stride within selected argument snapshots.")
    parser.add_argument("--max-follow-pointers", type=int, default=64, help="Maximum child pointers to follow per selected argument per call.")
    parser.add_argument("--out-dir", default="tools/live_captures/ipc_decode")
    args = parser.parse_args()

    try:
        import frida  # type: ignore
    except Exception as exc:
        raise SystemExit(f"frida package not available: {exc}")

    if args.pid is None:
        import subprocess

        args.pid = int(
            subprocess.check_output(
                ["powershell", "-NoProfile", "-Command", "(Get-Process FL64 -ErrorAction Stop | Select-Object -First 1).Id"],
                text=True,
            ).strip()
        )

    snapshot_args = [int(x.strip()) for x in args.snapshot_args.split(",") if x.strip()]
    target = parse_target(args.target)

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    jsonl_path = out_dir / f"izotope_state_diff_trace_{stamp}.jsonl"
    bin_dir = out_dir / f"izotope_state_diff_trace_{stamp}_bins"
    summary_path = out_dir / f"izotope_state_diff_trace_{stamp}_summary.json"
    bin_dir.mkdir(parents=True, exist_ok=True)

    print(f"Attaching to PID {args.pid}")
    print(f"Hooking {args.target}")
    print(f"Snapshot args: {snapshot_args}; bytes per snapshot: {args.snapshot_bytes}")
    print(f"Writing JSONL to {jsonl_path}")
    print("Trigger the Ozone Assistant or linked-plugin action while this runs.")

    stopped = False

    def stop_now(*_: Any) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_now)
    session = frida.get_local_device().attach(args.pid)
    script = session.create_script(
        make_js(
            target,
            snapshot_args,
            args.snapshot_bytes,
            args.max_ranges,
            args.heartbeat_every,
            args.follow_pointers,
            args.follow_bytes,
            args.follow_stride,
            args.max_follow_pointers,
        )
    )

    event_index = 0

    with jsonl_path.open("w", encoding="utf-8") as fp:
        def on_message(message: dict[str, Any], data: bytes | None) -> None:
            nonlocal event_index
            payload = message.get("payload", {}) if message.get("type") == "send" else message
            if data:
                event_index += 1
                digest = hashlib.sha256(data).hexdigest()
                label = safe_filename(str(payload.get("label", "event")))
                arg = payload.get("arg_index", "x")
                ptr = safe_filename(str(payload.get("pointer", "ptr")))
                bin_name = f"{event_index:06d}_{payload.get('kind','event')}_{label}_arg{arg}_{ptr}_{digest[:12]}.bin"
                bin_path = bin_dir / bin_name
                bin_path.write_bytes(data)
                payload["snapshot_file"] = str(bin_path)
                payload["snapshot_sha256"] = digest
                payload["snapshot_len"] = len(data)
                payload["snapshot_ascii_preview"] = ascii_preview(data)
                payload["snapshot_hex_preview"] = hex_preview(data)

            fp.write(json.dumps(payload, separators=(",", ":")) + "\n")
            fp.flush()

            kind = payload.get("kind")
            if kind in {"target_hook", "target_missing_module", "trace_ready", "state_initial", "state_changed", "child_state_initial", "child_state_changed", "call_heartbeat"}:
                brief = {
                    k: payload.get(k)
                    for k in ("kind", "label", "module", "rva", "address", "call_index", "arg_index", "parent_offset", "pointer", "changed_bytes", "retval")
                    if k in payload
                }
                print(json.dumps(brief, separators=(",", ":")))

        script.on("message", on_message)
        script.load()
        end = time.time() + max(0.1, args.duration)
        while not stopped and time.time() < end:
            time.sleep(0.2)

    try:
        script.unload()
    finally:
        session.detach()

    summary = summarize(jsonl_path)
    summary["jsonl_path"] = str(jsonl_path)
    summary["snapshot_dir"] = str(bin_dir)
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Summary written to {summary_path}")
    print(json.dumps(summary, indent=2)[:6000])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
