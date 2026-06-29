#!/usr/bin/env python3
"""Self-contained runner: launch OzoneHeadlessHost, attach Frida observe-only,
load ozone_live_context.js, collect the live controller context, then kill the
host. Hard-capped so it never hangs."""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path
import frida

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_live_context.js")
OUT = ROOT / "tools" / "live_captures" / "live_context.json"


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "22"
    host = subprocess.Popen([EXE], stderr=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, env=env)
    print(f"host pid={host.pid}")
    msgs = []
    try:
        time.sleep(2.0)
        if host.poll() is not None:
            print("host exited early")
            return 1
        session = frida.get_local_device().attach(host.pid)
        script = session.create_script(Path(JS).read_text(encoding="utf-8"))

        def on_message(m, d):
            if m.get("type") == "send":
                p = m["payload"]
                print("  " + json.dumps(p))
                msgs.append(p)
            elif m.get("type") == "error":
                print("  ERR:", m.get("description"))

        script.on("message", on_message)
        script.load()
        # wait until summary or host exits
        deadline = time.time() + 16
        while time.time() < deadline and host.poll() is None:
            if any(isinstance(m, dict) and m.get("kind") == "summary" for m in msgs):
                break
            time.sleep(0.3)
        try:
            script.unload()
        except Exception:
            pass
        session.detach()
    finally:
        try:
            host.kill()
        except Exception:
            pass
        try:
            host.wait(timeout=4)
        except Exception:
            pass

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(msgs, indent=2), encoding="utf-8")
    print(f"\nsaved -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
