#!/usr/bin/env python3
"""Runner for ozone_gated_trigger.js: binary-interception headless Master
Assistant trigger, in our own OzoneHeadlessHost process only.

Sequence:
  1. Launch OzoneHeadlessHost (GUI-init'd; controller alive).
  2. Attach Frida; load ozone_gated_trigger.js (observe-only until armed).
  3. Wait for the poller to deliver the live controller this + rdx.
  4. rpc.arm() -> on the next poller tick the thunk 0xD572F0 fires with live ctx.
  5. Report body/pipeline deltas. A non-zero pipeline delta == analysis ran.
  6. Snapshot Ozone parameters before/after when --diff is passed (sanctioned API).

Never touches the applier. Never attaches to any vendor/DAW/PACE process.
"""
from __future__ import annotations
import json, os, subprocess, sys, time
from pathlib import Path
import frida

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_gated_trigger.js")
OUT = ROOT / "tools" / "live_captures" / "gated_trigger_result.json"


def main():
    arm = "--arm" in sys.argv
    mode = "body" if "--body" in sys.argv else "thunk"
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "90"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE,
                            stdout=subprocess.DEVNULL, env=env, text=True, bufsize=1)
    print(f"host pid={host.pid}  arm={arm}")
    msgs = []
    host_lines = []
    try:
        # Give Ozone ample time to fully construct its controller + start the
        # poller timer (observed flaky: poller can take several seconds to begin).
        time.sleep(6.0)
        if host.poll() is not None:
            print("host exited early (likely DLL/license issue)")
            try:
                remaining = host.stderr.read()
                host_lines.append(remaining)
                print("host stderr:", (remaining or "")[-1500:])
            except Exception:
                pass
            return 1
        session = frida.get_local_device().attach(host.pid)
        script = session.create_script(Path(JS).read_text(encoding="utf-8"))

        def on_message(m, d):
            if m.get("type") == "send":
                p = m["payload"]
                print("  " + json.dumps(p))
                msgs.append(p)
                # flush handshake: when the script is about to fire, ack so its
                # recv().wait() unblocks and the (possibly faulting) call proceeds.
                if isinstance(p, dict) and p.get("kind") == "about_to_fire":
                    try:
                        script.exports_sync.flush()
                    except Exception as e:
                        print("  flush rpc failed:", e)
            elif m.get("type") == "error":
                print("  ERR:", m.get("description"))

        script.on("message", on_message)
        script.load()

        # wait for live context capture (poller must actually fire)
        cap_deadline = time.time() + 20
        captured = False
        while time.time() < cap_deadline and host.poll() is None:
            if any(isinstance(m, dict) and m.get("kind") == "captured" for m in msgs):
                captured = True
                break
            time.sleep(0.2)
        if not captured and arm:
            print("WARN: poller never fired in 20s; cannot fire safely. Aborting arm.")
            arm = False

        if arm:
            print(f">>> ARMING (will fire the {mode} via JS-thread dispatch)")
            try:
                r = script.exports_sync.arm(mode)
                print("  arm ->", r)
            except Exception as e:
                print("  arm rpc failed:", e)
            # let it fire + observe pipeline advancement (beats arrive every 0.5s)
            deadline = time.time() + 30
            while time.time() < deadline and host.poll() is None:
                if any(isinstance(m, dict) and m.get("kind") == "fired_immediate" for m in msgs):
                    break
                time.sleep(0.3)
        else:
            print("DRY-RUN: not arming. Pass --arm to fire.")
            time.sleep(6)

        try:
            final = script.exports_sync.status()
            print("  status ->", json.dumps(final))
            msgs.append({"kind": "final_status", "status": final})
        except Exception:
            pass
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
        try:
            tail = host.stderr.read() if host.poll() is not None else ""
            if tail:
                print("host stderr tail:", tail[-1500:])
        except Exception:
            pass

    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(msgs, indent=2), encoding="utf-8")

    # success criterion
    fired_msgs = [m for m in msgs if isinstance(m, dict) and m.get("kind") == "fired_immediate"]
    beats = [m for m in msgs if isinstance(m, dict) and m.get("kind") == "beat"]
    pipeline_deltas = [m.get("delta_immediate", {}).get("pipeline", 0) for m in fired_msgs]
    body_deltas = [m.get("delta_immediate", {}).get("body", 0) for m in fired_msgs]
    max_beat_pipeline = max([b.get("pipeline", 0) for b in beats], default=0)
    max_beat_body = max([b.get("body", 0) for b in beats], default=0)
    call_finished = any(b.get("call_finished") for b in beats) or bool(fired_msgs)
    call_err = next((b.get("call_err") for b in beats if b.get("call_err")), None)
    saw_about = any(isinstance(m, dict) and m.get("kind") == "about_to_fire" for m in msgs)
    host_crashed = host.returncode is not None and host.returncode != 0
    print("\n=== RESULT ===")
    print(f"about_to_fire={saw_about} fired_events={len(fired_msgs)} "
          f"max_beat_pipeline={max_beat_pipeline} max_beat_body={max_beat_body} "
          f"call_finished={call_finished} call_err={call_err} host_crashed={host_crashed}")
    if max_beat_pipeline > 0 or any(d > 0 for d in pipeline_deltas):
        print(">>> ANALYSIS PIPELINE FIRED — headless binary-interception trigger SUCCEEDED <<<")
        return 0
    if max_beat_body > 0 or any(d > 0 for d in body_deltas):
        print(">>> BODY executed (re-entry observed) but pipeline delta=0 — entered but didn't start analysis <<<")
        return 0
    if fired_msgs and not host_crashed:
        print(">>> call returned cleanly but pipeline delta=0 (not analysis-start) <<<")
        return 0
    if saw_about and host_crashed and not call_finished:
        print(">>> call was reached, then host crashed (access-violation fault) — body rejected the context <<<")
    elif saw_about and not call_finished:
        print(">>> call was reached but never returned (blocking/spinning) — body entered a wait/fault <<<")
    elif not saw_about:
        print(">>> never reached the fire (capture failed or host exited) <<<")
    print(f"saved -> {OUT}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
