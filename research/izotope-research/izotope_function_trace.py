#!/usr/bin/env python3
"""
Trace selected internal iZotope functions by module-relative RVA.

This is observational Frida instrumentation. It logs calls, arguments, return
values, and short memory previews for pointer-looking arguments.

Default targets are derived from the Ozone Master Assistant state-table access
found by tools/izotope_data_watch.py:
  - iZOzonePro.dll+0xEAD3E0  state-processing function
  - iZOzonePro.dll+0xEAD2E0  helper called before state-table lookup
  - iZOzonePro.dll+0xEAD360  helper called before state-table lookup

Run it, trigger Ozone Assistant/link activity, then inspect the JSONL.
"""

from __future__ import annotations

import argparse
import json
import signal
import time
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any


DEFAULT_TARGETS = [
    "iZOzonePro.dll+0xEAD3E0:ozone_master_assistant_state",
    "iZOzonePro.dll+0xEAD2E0:ozone_state_helper_a",
    "iZOzonePro.dll+0xEAD360:ozone_state_helper_b",
]


JS_TEMPLATE = r"""
'use strict';

const TARGETS = __TARGETS__;
const MAX_BYTES = __MAX_BYTES__;

function emit(kind, data, ptr, len) {
  data = data || {};
  data.kind = kind;
  data.ts_ms = Date.now();
  if (ptr && len && len > 0) {
    try {
      send(data, ptr.readByteArray(Math.min(len, MAX_BYTES)));
      return;
    } catch (e) {
      data.read_error = String(e);
    }
  }
  send(data);
}

function safeUtf8(p) {
  try {
    if (p.isNull()) return '';
    return p.readUtf8String(160) || '';
  } catch (_) { return ''; }
}

function safeUtf16(p) {
  try {
    if (p.isNull()) return '';
    return p.readUtf16String(160) || '';
  } catch (_) { return ''; }
}

function safeHex(p, len) {
  try {
    if (p.isNull()) return '';
    const data = p.readByteArray(len);
    if (!data) return '';
    const u = new Uint8Array(data);
    return Array.prototype.map.call(u, x => ('0' + x.toString(16)).slice(-2)).join(' ');
  } catch (_) { return ''; }
}

function moduleInfo(addr) {
  try {
    const m = Process.findModuleByAddress(addr);
    if (!m) return null;
    return { name: m.name, path: m.path, base: String(m.base), rva: '0x' + ptr(addr).sub(m.base).toString(16) };
  } catch (_) { return null; }
}

function backtrace(context) {
  try {
    return Thread.backtrace(context, Backtracer.ACCURATE).slice(0, 16).map(function (addr) {
      const s = DebugSymbol.fromAddress(addr);
      return { address: String(addr), module: s.moduleName || '', name: s.name || '', symbol: String(s) };
    });
  } catch (e) {
    return [{ error: String(e) }];
  }
}

function describeArg(v) {
  const p = ptr(v);
  const rec = { value: String(p) };
  const u8 = safeUtf8(p);
  const u16 = safeUtf16(p);
  if (u8 && /^[\x09\x0a\x0d\x20-\x7e]{3,}/.test(u8)) rec.utf8 = u8;
  if (u16 && /^[\x09\x0a\x0d\x20-\x7e]{3,}/.test(u16)) rec.utf16 = u16;
  rec.hex = safeHex(p, 48);
  return rec;
}

const modules = {};
Process.enumerateModules().forEach(function (m) {
  modules[m.name.toLowerCase()] = m;
  if (/izotope|ozone|neutron|relay|morephi/i.test(m.name + ' ' + m.path)) {
    emit('module', { name: m.name, path: m.path, base: String(m.base), size: m.size });
  }
});

TARGETS.forEach(function (t) {
  const m = modules[t.module.toLowerCase()];
  if (!m) {
    emit('target_missing_module', t);
    return;
  }
  const addr = m.base.add(t.rva);
  emit('target_hook', { module: m.name, rva: '0x' + t.rva.toString(16), address: String(addr), label: t.label });
  Interceptor.attach(addr, {
    onEnter(args) {
      this.label = t.label;
      this.addr = addr;
      const argRecs = [];
      for (let i = 0; i < 6; ++i) argRecs.push(describeArg(args[i]));
      emit('call_enter', {
        label: t.label,
        module: m.name,
        rva: '0x' + t.rva.toString(16),
        thread_id: Process.getCurrentThreadId(),
        return_address: String(this.returnAddress),
        return_module: moduleInfo(this.returnAddress),
        args: argRecs,
        backtrace: backtrace(this.context)
      });
    },
    onLeave(retval) {
      emit('call_leave', {
        label: this.label,
        retval: String(retval),
        retval_arg: describeArg(retval)
      });
    }
  });
});

emit('trace_ready', { targets: TARGETS.length });
"""


def parse_target(raw: str) -> dict[str, Any]:
    label = ""
    spec = raw
    if ":" in raw:
        spec, label = raw.split(":", 1)
    module, rva_text = spec.split("+", 1)
    return {"module": module, "rva": int(rva_text, 16), "label": label or spec}


def ascii_preview(data: bytes | None) -> str:
    if not data:
        return ""
    return "".join(chr(b) if 32 <= b < 127 else "." for b in data[:256])


def make_js(targets: list[dict[str, Any]], max_bytes: int) -> str:
    return JS_TEMPLATE.replace("__TARGETS__", json.dumps(targets, separators=(",", ":"))).replace("__MAX_BYTES__", str(max_bytes))


def summarize(path: Path) -> dict[str, Any]:
    counts = Counter()
    calls = Counter()
    returns = Counter()
    samples = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        ev = json.loads(line)
        counts[ev.get("kind", "")] += 1
        if ev.get("kind") == "call_enter":
            calls[ev.get("label", "")] += 1
            samples.setdefault(ev.get("label", ""), ev)
        elif ev.get("kind") == "call_leave":
            returns[ev.get("label", "")] += 1
    return {
        "event_counts": dict(counts.most_common()),
        "calls": dict(calls.most_common()),
        "returns": dict(returns.most_common()),
        "samples": samples,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, help="Target PID. Defaults to first FL64.exe.")
    parser.add_argument("--target", action="append", default=[], help="module.dll+0xRVA[:label]")
    parser.add_argument("--duration", type=float, default=45.0)
    parser.add_argument("--max-bytes", type=int, default=256)
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

    targets = [parse_target(t) for t in (args.target or DEFAULT_TARGETS)]
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    jsonl_path = out_dir / f"izotope_function_trace_{stamp}.jsonl"
    summary_path = out_dir / f"izotope_function_trace_{stamp}_summary.json"

    print(f"Attaching to PID {args.pid}")
    print(f"Hooking {len(targets)} internal functions")
    print(f"Writing events to {jsonl_path}")
    print("Trigger the iZotope Assistant/link action while this runs.")

    stopped = False

    def stop_now(*_: Any) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_now)
    session = frida.get_local_device().attach(args.pid)
    script = session.create_script(make_js(targets, args.max_bytes))

    with jsonl_path.open("w", encoding="utf-8") as fp:
        def on_message(message: dict[str, Any], data: bytes | None) -> None:
            payload = message.get("payload", {}) if message.get("type") == "send" else message
            if data:
                payload["payload_len"] = len(data)
                payload["payload_ascii_preview"] = ascii_preview(data)
            fp.write(json.dumps(payload, separators=(",", ":")) + "\n")
            fp.flush()
            if payload.get("kind") in {"target_hook", "target_missing_module", "trace_ready", "call_enter", "call_leave"}:
                brief = {k: payload.get(k) for k in ("kind", "label", "module", "rva", "address", "retval") if k in payload}
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
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Summary written to {summary_path}")
    print(json.dumps(summary, indent=2)[:6000])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
