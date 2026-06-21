#!/usr/bin/env python3
"""Option 2a — enumerate Ozone Pro's public VST3 parameters to find ANY headless
trigger surface (assistant/analyze/learn/listen/run). Loads Ozone in a More-Phi-
owned process (MorePhiMcpServer) via the sanctioned host API. No Frida, no FL64.

Decisive fork:
 - if a trigger-like param exists -> arm it + render + observe the real trigger.
 - if none -> headless trigger is not reachable via public params; document.
"""
from __future__ import annotations
import json, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "build", "Release", "MorePhiMcpServer.exe")
VST3 = r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"
TRIGGER_WORDS = ("assistant", "analyze", "analyse", "learn", "listen", "run", "start", "play",
                 "master ass", "enable", "arming", "calc", "detect")


def main():
    env = dict(os.environ)
    env["OZONE_VST3_PATH"] = VST3
    p = subprocess.Popen([EXE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, text=True, env=env, bufsize=1)
    req_id = [0]

    def call(method, params=None):
        req_id[0] += 1
        req = {"jsonrpc": "2.0", "id": req_id[0], "method": method, "params": params or {}}
        p.stdin.write(json.dumps(req) + "\n")
        p.stdin.flush()
        deadline = time.time() + 60
        while time.time() < deadline:
            line = p.stdout.readline()
            if not line:
                break
            line = line.strip()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if msg.get("id") == req_id[0]:
                return msg
        return None

    init = call("initialize")
    if not init:
        print("FATAL: no initialize response (Ozone/PACE may have blocked or crashed the host).")
        print("stderr:", (p.stderr.read() or "")[-2000:])
        return 1

    r = call("tools/call", {"name": "ozone_get_parameters", "arguments": {"include_values": False}})
    try:
        p.stdin.close()
    except Exception:
        pass

    if not r or "result" not in r:
        print("FATAL: no ozone_get_parameters response.")
        print("stderr:", (p.stderr.read() or "")[-2000:])
        return 1

    # result content is nested under result.content[].text or result itself; be tolerant
    result = r["result"]
    body = result
    if isinstance(result, dict) and result.get("content"):
        for c in result["content"]:
            if isinstance(c, dict) and "text" in c:
                try:
                    body = json.loads(c["text"])
                except Exception:
                    body = result
                break

    if body.get("isError") or not body.get("success", True):
        print("Ozone did NOT load cleanly:")
        print(json.dumps(body, indent=2)[:3000])
        return 2

    params = body.get("parameters", [])
    plugin = body.get("plugin", {})
    print(f"Ozone loaded: {plugin.get('name')} / {plugin.get('descriptive_name')} / {plugin.get('manufacturerName')}")
    print(f"total parameters: {body.get('count')} (returned {len(params)})")

    hits = []
    for prm in params:
        name = (prm.get("name") or "").lower()
        sid = (prm.get("stable_id") or "").lower()
        hay = name + " " + sid
        for w in TRIGGER_WORDS:
            if w in hay:
                hits.append((prm.get("index"), prm.get("name"), prm.get("stable_id"), w))
                break

    print(f"\n=== parameters matching trigger words ({len(hits)}) ===")
    for idx, name, sid, w in hits:
        print(f"  idx={idx}  {name!r}  [{sid}]  match={w!r}")

    if not hits:
        print("  (none) -> no public assistant/analyze/learn param exposed; headless trigger")
        print("           cannot be seeded via the public VST3 parameter surface.")

    # also: any param with 'assist' substring anywhere
    assist = [p.get("name") for p in params if "assist" in (p.get("name", "") + p.get("stable_id", "")).lower()]
    print(f"\n'assist' substring params: {assist[:20]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
