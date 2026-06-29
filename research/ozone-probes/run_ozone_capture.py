#!/usr/bin/env python3
"""Phase A driver — observe-only capture of the live Ozone Master Assistant
controller pointer from the idle poller, in OUR host process. No calls, no
mutation, no FL64. Prints captured 'this' candidates with vtable RVAs.
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "MorePhiMcpServer.exe")
VST3 = r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"
JS = str(ROOT / "tools" / "ozone_capture_controller.js")
WAV = str(ROOT / "tools" / "live_captures" / "static" / "test_tone.wav")
# assistant_parameter_index 33 = "EQ: St/M/L Enable 1" (benign); forces runMasterAssistant
# to render audio through Ozone's processBlock (which wakes the assistant controller) even
# though no real trigger parameter exists.
DRIVE_PARAM_INDEX = 33

import frida  # noqa: E402


def mcp_call(host, req_id, method, params=None, timeout=120):
    req = {"jsonrpc": "2.0", "id": req_id, "method": method, "params": params or {}}
    host.stdin.write(json.dumps(req) + "\n")
    host.stdin.flush()
    deadline = time.time() + timeout
    while time.time() < deadline:
        line = host.stdout.readline()
        if not line:
            return None
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        if msg.get("id") == req_id:
            return msg
    return None


def main():
    env = dict(os.environ)
    env["OZONE_VST3_PATH"] = VST3
    env["OZONE_OPEN_EDITOR"] = "1"  # construct Ozone's editor headlessly to wake the assistant controller
    host = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, env=env, text=True, bufsize=1)
    print(f"host pid={host.pid} (MorePhiMcpServer) — our process")
    try:
        time.sleep(3.0)  # let Ozone load + PACE settle
        if host.poll() is not None:
            print("FATAL: host exited early. stderr:", (host.stderr.read() or b"")[-1500:])
            return 1

        mcp_call(host, 1, "initialize")

        session = frida.get_local_device().attach(host.pid)
        script = session.create_script(Path(JS).read_text(encoding="utf-8"))
        hits, tallies, hooks = [], [], []

        def on_message(msg, data):
            if msg.get("type") != "send":
                print("MSG:", msg)
                return
            p = msg["payload"]
            k = p.get("kind")
            if k == "hit":
                hits.append(p["rec"])
            elif k == "tally":
                tallies.append(p["counts"])
            elif k in ("module_found", "hook_ok", "hook_fail", "all_hooked"):
                hooks.append(p)
                print("  ", json.dumps(p))

        script.on("message", on_message)
        script.load()
        time.sleep(2.0)  # idle window — see if poller self-fires

        print(f"\ndriving audio through Ozone (param {DRIVE_PARAM_INDEX}, 20s) to wake controller...")
        r = mcp_call(host, 2, "tools/call", {"name": "ozone_run_master_assistant",
                  "arguments": {"assistant_parameter_index": DRIVE_PARAM_INDEX,
                                "input_audio_path": WAV, "analysis_seconds": 20.0}}, timeout=180)
        print("render result:", json.dumps(r)[:600] if r else "NO RESPONSE")
        time.sleep(2.0)  # trailing window

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
            err = host.stderr.read()
        except Exception:
            err = ""
        print("\n=== host stderr tail ===")
        print((err or "")[-2500:])

    print("\n=== final tally (call counts per hook) ===")
    print(json.dumps(tallies[-1] if tallies else {}, indent=2))

    # Dedup controller candidates by vtable RVA (the trigger's 'this' has an in-module vtable).
    by_vt = {}
    for h in hits:
        rcx = h.get("rcx", {})
        if rcx.get("vtable_in_mod"):
            key = (h["label"], rcx.get("vtable_rva"), rcx.get("deref0x90"))
            by_vt.setdefault(key, []).append(h)

    print(f"\n=== captured 'this' candidates with in-module vtable ({len(by_vt)}) ===")
    for (label, vt_rva, d90), recs in sorted(by_vt.items(), key=lambda kv: kv[0][0]):
        r = recs[0]
        print(f"  [{label}] this={r['rcx']['ptr']} vtable_rva={vt_rva} [+0x90]={d90} "
              f"rdx={r.get('rdx')} r8={r.get('r8')} (n={len(recs)})")

    if not by_vt:
        print("  (none with in-module vtable captured). Raw hits:")
        for h in hits[:20]:
            print("  ", json.dumps(h))

    return 0


if __name__ == "__main__":
    sys.exit(main())
