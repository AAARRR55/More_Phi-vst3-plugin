#!/usr/bin/env python3
"""Drive ozone_probe_controller.js against OzoneHeadlessHost: read-only explore of
the live controller object to locate the trigger's owning vtable/object."""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "tools" / "ozone_probe_controller.js")
print(f"using JS: {JS}")

import frida  # noqa: E402


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "16"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, env=env, text=True, bufsize=1)
    print(f"host pid={host.pid}")
    try:
        time.sleep(2.0)
        session = frida.get_local_device().attach(host.pid)
        script = session.create_script(Path(JS).read_text(encoding="utf-8"))
        reports = []

        def on_message(msg, data):
            if msg.get("type") != "send":
                return
            p = msg["payload"]
            if p.get("kind") == "controller":
                reports.append(p["report"])
            elif p.get("kind") in ("mod", "ready"):
                print("  ", json.dumps(p))

        script.on("message", on_message)
        script.load()
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
            pass

    if not reports:
        print("No controller captured.")
        return 1
    r = reports[0]
    print("\n=== object A (poller this) ===")
    print(json.dumps(r.get("objA"), indent=2))
    subs = r.get("sub_objects", [])
    hits = [s for s in subs if s.get("has_trigger")]
    print(f"\n=== sub-objects scanned: {len(subs)}; owning the TRIGGER: {len(hits)} ===")
    for s in hits:
        print(json.dumps(s, indent=2))
    if not subs:
        print("(no in-module-vtable sub-objects found in object A's first 0x400 bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
