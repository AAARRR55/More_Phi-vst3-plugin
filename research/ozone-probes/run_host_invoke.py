#!/usr/bin/env python3
"""Drive ozone_invoke_trigger.js against OzoneHeadlessHost.
Default = DRY-RUN (locate trigger objects + capture applier context, NO call).
Pass --invoke to actually call the trigger thunk on each candidate object.
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_invoke_trigger.js")
INVOKE = "--invoke" in sys.argv

import frida  # noqa: E402


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "30" if INVOKE else "18"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, env=env, text=True, bufsize=1)
    print(f"host pid={host.pid}  mode={'INVOKE (will call trigger!)' if INVOKE else 'DRY-RUN'}")
    try:
        time.sleep(2.0)
        if host.poll() is not None:
            print("host exited early:", (host.stderr.read() or "")[-1000:])
            return 1
        session = frida.get_local_device().attach(host.pid)
        script = session.create_script(Path(JS).read_text(encoding="utf-8"))

        def on_message(msg, data):
            if msg.get("type") == "send":
                print("  " + json.dumps(msg["payload"]))
            else:
                print("  MSG:", msg)
        script.on("message", on_message)
        script.load()
        if INVOKE:
            print(">>> enabling gated invocation")
            try:
                script.exports_sync.enable_invoke()
            except Exception as e:
                print("rpc enable_invoke failed:", e)
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
        try:
            err = host.stderr.read()
        except Exception:
            err = ""
        print("\n=== host stderr tail ===")
        print((err or "")[-1500:])
    return 0


if __name__ == "__main__":
    sys.exit(main())
