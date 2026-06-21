#!/usr/bin/env python3
"""Launch OzoneHeadlessHost, attach Frida, load ozone_disasm_static.js, print
the one-shot static disasm of controller RVAs, exit. No calls, no mutation."""
from __future__ import annotations
import json, os, subprocess, time
from pathlib import Path
import frida

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_disasm_static.js")


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "14"
    host = subprocess.Popen([EXE], stderr=subprocess.DEVNULL, env=env)
    time.sleep(3.0)
    if host.poll() is not None:
        print("host exited early"); return 1
    session = frida.get_local_device().attach(host.pid)
    script = session.create_script(Path(JS).read_text(encoding="utf-8"))
    got = []
    def on_message(msg, data):
        if msg.get("type") == "send" and msg["payload"].get("kind") == "disasm":
            got.append(msg["payload"]["data"])
    script.on("message", on_message)
    script.load()
    for _ in range(60):
        if got: break
        time.sleep(0.2)
    try: script.unload()
    except Exception: pass
    session.detach()
    try: host.kill()
    except Exception: pass
    if not got:
        print("no disasm message received"); return 1
    d = got[0]
    print("base:", d.get("base"))
    for k in ["poller", "state_helper_a", "state_helper_b", "hub", "applier", "getter", "pipeline_root", "caller"]:
        print("\n=== " + k + " ===")
        for line in d.get(k, []):
            print("  " + line)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
