#!/usr/bin/env python3
"""
Watch iZotope IPC/Assistant string-table pages for runtime reads.

This is the next stage after tools/izotope_ipc_xref.py when direct code xrefs
are absent. It uses Frida MemoryAccessMonitor on a small set of pages containing
interesting iZotope data-table pointers, then reports the code address (`from`)
that accessed those pages.

The monitor is observational only. It does not patch code or write memory.
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


JS_TEMPLATE = r"""
'use strict';

const WATCH = __WATCH__;

function emit(kind, data) {
  data = data || {};
  data.kind = kind;
  data.ts_ms = Date.now();
  send(data);
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

function debugSymbol(addr) {
  try {
    const s = DebugSymbol.fromAddress(addr);
    return String(s);
  } catch (_) {
    return '';
  }
}

function safeHex(addr, len) {
  try {
    const data = ptr(addr).readByteArray(len);
    if (!data) return '';
    const u = new Uint8Array(data);
    return Array.prototype.map.call(u, x => ('0' + x.toString(16)).slice(-2)).join(' ');
  } catch (e) {
    return '<read error: ' + e + '>';
  }
}

const active = [];
const modules = {};
Process.enumerateModules().forEach(function (m) {
  modules[m.name.toLowerCase()] = m;
  emit('module', { name: m.name, path: m.path, base: String(m.base), size: m.size });
});

WATCH.forEach(function (w) {
  const m = modules[w.module.toLowerCase()];
  if (!m) {
    emit('watch_missing_module', w);
    return;
  }
  const base = m.base.add(w.page_rva);
  active.push({ base: base, size: w.size, spec: w, module: m });
  emit('watch_enabled', {
    module: m.name,
    page_rva: '0x' + w.page_rva.toString(16),
    base: String(base),
    size: w.size,
    patterns: w.patterns,
    refs: w.refs
  });
});

if (active.length > 0) {
  MemoryAccessMonitor.enable(active.map(x => ({ base: x.base, size: x.size })), {
    onAccess(details) {
      let hit = null;
      for (const item of active) {
        if (details.address.compare(item.base) >= 0 && details.address.compare(item.base.add(item.size)) < 0) {
          hit = item;
          break;
        }
      }
      const fromMod = moduleInfo(details.from);
      const addrMod = moduleInfo(details.address);
      emit('data_access', {
        operation: details.operation,
        address: String(details.address),
        address_module: addrMod,
        from: String(details.from),
        from_module: fromMod,
        from_symbol: debugSymbol(details.from),
        range_module: hit ? hit.module.name : '',
        range_page_rva: hit ? '0x' + hit.spec.page_rva.toString(16) : '',
        patterns: hit ? hit.spec.patterns : [],
        refs: hit ? hit.spec.refs : [],
        bytes: safeHex(details.address, 64)
      });
    }
  });
}

emit('watch_ready', { count: active.length });
"""


def load_watch_specs(xref_path: Path, patterns: set[str], max_pages: int) -> list[dict[str, Any]]:
    data = json.loads(xref_path.read_text(encoding="utf-8"))
    pages: dict[tuple[str, int], dict[str, Any]] = {}
    for module in data.get("modules", []):
        module_name = module.get("module_name")
        if not module_name:
            continue
        for ref in module.get("pointer_refs", []):
            pat = ref.get("target_pattern", "")
            if patterns and pat not in patterns:
                continue
            page_rva = int(ref["rva"]) & ~0xFFF
            key = (module_name, page_rva)
            entry = pages.setdefault(
                key,
                {"module": module_name, "page_rva": page_rva, "size": 0x1000, "patterns": Counter(), "refs": []},
            )
            entry["patterns"][pat] += 1
            if len(entry["refs"]) < 10:
                entry["refs"].append(
                    {
                        "ptr_rva": f"0x{int(ref['rva']):X}",
                        "target_rva": f"0x{int(ref['target_rva']):X}",
                        "pattern": pat,
                        "kind": ref.get("kind"),
                    }
                )
    specs = list(pages.values())
    for spec in specs:
        spec["patterns"] = [p for p, _ in spec["patterns"].most_common()]
    specs.sort(key=lambda s: (len(s["refs"]), ",".join(s["patterns"])), reverse=True)
    return specs[:max_pages]


def make_js(watch: list[dict[str, Any]]) -> str:
    return JS_TEMPLATE.replace("__WATCH__", json.dumps(watch, separators=(",", ":")))


def summarize(path: Path) -> dict[str, Any]:
    counts = Counter()
    callers = Counter()
    samples: dict[str, dict[str, Any]] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        ev = json.loads(line)
        counts[ev.get("kind", "")] += 1
        if ev.get("kind") == "data_access":
            fm = ev.get("from_module") or {}
            key = f"{fm.get('name','?')}:{fm.get('rva','?')}"
            callers[key] += 1
            samples.setdefault(key, ev)
    return {
        "event_counts": dict(counts.most_common()),
        "callers": [
            {
                "caller": key,
                "count": count,
                "symbol": samples[key].get("from_symbol"),
                "patterns": samples[key].get("patterns"),
                "range_module": samples[key].get("range_module"),
                "range_page_rva": samples[key].get("range_page_rva"),
            }
            for key, count in callers.most_common(80)
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pid", type=int, help="Target PID. Defaults to first FL64.exe.")
    parser.add_argument("--xref", default="tools/live_captures/ipc_decode/izotope_ipc_xrefs_pointer.json")
    parser.add_argument("--duration", type=float, default=45.0)
    parser.add_argument("--max-pages", type=int, default=32)
    parser.add_argument("--pattern", action="append", default=[])
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

    default_patterns = {
        "Ozone IPC 1",
        "Neutron IPC 2",
        "SmoothAudioDataStream",
        "Smooth Audio Streamer",
        "AuxStream",
        "Track Assist Processor",
        "Balance Assistant Learner",
        "Master Assistant",
        "Master Assistant: Launched",
        "PROCESSING_LISTENING",
        "PROCESSING_SETTING_SIGNAL_CHAIN",
        "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    }
    patterns = set(args.pattern) if args.pattern else default_patterns
    watch = load_watch_specs(Path(args.xref), patterns, args.max_pages)
    if not watch:
        raise SystemExit("No watch pages selected.")

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    jsonl_path = out_dir / f"izotope_data_watch_{stamp}.jsonl"
    summary_path = out_dir / f"izotope_data_watch_{stamp}_summary.json"

    print(f"Attaching to PID {args.pid}")
    print(f"Watching {len(watch)} iZotope data pages")
    print(f"Writing events to {jsonl_path}")
    print("Trigger the iZotope linked/Assistant action while this runs.")

    stopped = False

    def stop_now(*_: Any) -> None:
        nonlocal stopped
        stopped = True

    signal.signal(signal.SIGINT, stop_now)
    session = frida.get_local_device().attach(args.pid)
    script = session.create_script(make_js(watch))
    with jsonl_path.open("w", encoding="utf-8") as fp:
        def on_message(message: dict[str, Any], data: bytes | None) -> None:
            payload = message.get("payload", {}) if message.get("type") == "send" else message
            fp.write(json.dumps(payload, separators=(",", ":")) + "\n")
            fp.flush()
            if payload.get("kind") in {"watch_ready", "watch_enabled", "data_access", "watch_missing_module"}:
                brief = {k: payload.get(k) for k in ("kind", "module", "page_rva", "count", "from_symbol", "patterns", "range_module", "range_page_rva") if k in payload}
                if payload.get("kind") == "data_access":
                    brief["from_module"] = payload.get("from_module")
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
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
