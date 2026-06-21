#!/usr/bin/env python3
"""Capture live Master Assistant controller pointers from OzoneHeadlessHost
(the GUI-initialized host where the editor + controller actually exist, unlike
the stdio MCP server). Observe-only; our process only; no calls, no mutation.
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")
HOST_SECONDS = "22"

import frida  # noqa: E402


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = HOST_SECONDS
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, env=env, text=True, bufsize=1)
    print(f"host pid={host.pid} (OzoneHeadlessHost) — GUI-initialized, our process")
    try:
        time.sleep(2.0)  # let Ozone load + editor construct
        if host.poll() is not None:
            print("FATAL: host exited early. stderr:", (host.stderr.read() or "")[-1500:])
            return 1

        session = frida.get_local_device().attach(host.pid)
        script = session.create_script(Path(JS).read_text(encoding="utf-8"))
        hits, tallies = [], []

        def on_message(msg, data):
            if msg.get("type") != "send":
                return
            p = msg["payload"]
            if p.get("kind") == "hit":
                hits.append(p["rec"])
            elif p.get("kind") == "tally":
                tallies.append(p["counts"])
            elif p.get("kind") in ("module_found", "hook_ok", "hook_fail", "all_hooked"):
                print("  ", json.dumps(p))

        script.on("message", on_message)
        script.load()
        # host runs HOST_SECONDS; wait for it (it self-feeds audio + runs the message loop)
        while host.poll() is None:
            time.sleep(0.5)
        try:
            script.unload()
        except Exception:
            pass
        session.detach()
    finally:
        try:
            host.wait(timeout=3)
        except Exception:
            try:
                host.kill()
            except Exception:
                pass
        try:
            err = host.stderr.read()
        except Exception:
            err = ""
        print("\n=== host stderr tail ===")
        print((err or "")[-1800:])

    print("\n=== final tally (call counts per hook) ===")
    print(json.dumps(tallies[-1] if tallies else {}, indent=2))

    by_vt = {}
    for h in hits:
        rcx = h.get("rcx", {})
        if rcx.get("vtable_in_mod"):
            key = (h["label"], rcx.get("vtable_rva"), rcx.get("deref0x90"))
            by_vt.setdefault(key, []).append(h)

    print(f"\n=== captured live 'this' candidates (in-module vtable) ({len(by_vt)}) ===")
    for (label, vt_rva, d90), recs in sorted(by_vt.items()):
        r = recs[0]
        print(f"  [{label}] this={r['rcx']['ptr']} vtable_rva={vt_rva} [+0x90]={d90} "
              f"rdx={r.get('rdx')} r8={r.get('r8')} (n={len(recs)})")

    if not by_vt and hits:
        print("  raw hits:")
        for h in hits[:15]:
            print("  ", json.dumps(h))
    elif not hits:
        print("  (no hooks fired — controller not active even with editor constructed)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
