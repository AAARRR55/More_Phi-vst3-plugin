#!/usr/bin/env python3
"""Headless Ozone Master Assistant trigger via UI Automation InvokePattern.

This is the canonical accessibility-driven path: instead of fake mouse clicks
(which Ozone's view manager rejects — the Assistant view never opens under
PostMessage or even real mouse_event when the window is offscreen/), we drive
the controls through their UI Automation *Invoke* pattern. Invoke is exactly
the action a screen reader / accessibility client uses to activate a control;
it is the vendor-sanctioned programmatic activation surface and needs no
human, no onscreen presence, and no coordinate translation.

Flow:
  1. Launch OzoneHeadlessHost (GUI-init'd; editor desktoped offscreen).
  2. Attach Frida (observe-only) to tally the Master-Assistant pipeline.
  3. Find the "Master Assistant" button, Invoke() it -> opens Assistant View.
  4. Re-scan for the analysis-start control (Play / Learn / Listen / Start),
     Invoke() it.
  5. Watch the Frida tally delta: pipeline_root / body firing => analysis ran.

No vendor/DAW process is ever attached. Frida only sees our own host pid.
"""
from __future__ import annotations
import ctypes, os, re, subprocess, sys, time
from ctypes import wintypes
from pathlib import Path

import frida  # noqa: E402
import uiautomation as ua  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")

u = ctypes.windll.user32
k = ctypes.windll.kernel32

# Names that the analysis-start control goes by across Ozone versions.
START_HINTS = ("play", "learn", "listen", "start", "begin", "analyze",
               "open assistant", "vibe", "target")
ASSISTANT_HINTS = ("master assistant", "assistant")


def foreground(hwnd: int) -> None:
    """Foreground an offscreen/hidden window via thread-input attachment."""
    fg = u.GetForegroundWindow()
    tfg = u.GetWindowThreadProcessId(fg, None)
    me = k.GetCurrentThreadId()
    u.AttachThreadInput(me, tfg, True)
    try:
        u.ShowWindow(hwnd, 9)            # SW_RESTORE
        u.SetForegroundWindow(hwnd)
    finally:
        u.AttachThreadInput(me, tfg, False)


def walk(c, depth=0, maxdepth=14):
    """Yield (depth, control) for the whole subtree (DFS), resilient to COM hiccups."""
    if depth > maxdepth:
        return
    try:
        kids = c.GetChildren()
    except Exception:
        kids = []
    for x in kids:
        yield depth, x
        yield from walk(x, depth + 1, maxdepth)


def name_of(c) -> str:
    try:
        return (c.Name or "").strip()
    except Exception:
        return ""


def type_of(c) -> str:
    try:
        return c.ControlType
    except Exception:
        return ""


def find_all(root, predicate):
    out = []
    for _d, c in walk(root):
        try:
            if predicate(c):
                out.append(c)
        except Exception:
            continue
    return out


def invoke(control) -> bool:
    """Invoke a control via InvokePattern. Returns True if invoked."""
    try:
        p = control.GetInvokePattern()
    except Exception:
        p = None
    if p is None:
        return False
    try:
        p.Invoke()
        return True
    except Exception as e:
        print(f"  invoke failed: {e}")
        return False


def dump_summary(root, label: str) -> dict:
    """Print named buttons + hint matches; return hint-match controls."""
    named, hits = [], []
    for d, c in walk(root):
        nm = name_of(c); ct = type_of(c)
        if not nm:
            continue
        try:
            r = c.BoundingRectangle
            rect = (r.left, r.top, r.right, r.bottom)
        except Exception:
            rect = None
        if ct == 50000:                       # buttons
            named.append(nm)
        low = nm.lower()
        if any(h in low for h in START_HINTS) and not any(a in low for a in ASSISTANT_HINTS):
            hits.append((d, ct, nm, rect))
    print(f"\n--- {label}: {len(named)} named buttons ---")
    print("  " + ", ".join(sorted(set(named))))
    print(f"--- {label}: {len(hits)} analysis-start candidates ---")
    for d, ct, nm, rect in hits:
        print(f"  {'  '*d}{ct} name={nm!r} rect={rect}")
    return {"hits": hits, "named": named}


def wait_hwnd(host, timeout=25.0):
    hwnd = None
    deadline = time.time() + timeout
    while time.time() < deadline and hwnd is None:
        line = host.stderr.readline()
        if not line:
            if host.poll() is not None:
                break
            continue
        m = re.search(r"OZONE_EDITOR_HWND=0x([0-9a-fA-F]+)", line)
        if m:
            hwnd = int(m.group(1), 16)
    return hwnd


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "150"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = wait_hwnd(host)
    if hwnd is None:
        print("FATAL: no editor HWND captured.")
        try: host.kill()
        except Exception: pass
        return 1
    print(f"editor HWND = 0x{hwnd:x}")

    # Move onscreen so visibility-gated view construction is satisfied.
    u.MoveWindow(hwnd, 30, 30, 1160, 600, True)
    foreground(hwnd)
    time.sleep(1.5)

    # Frida observe-only attach to our own host pid.
    session = frida.get_local_device().attach(host.pid)
    script = session.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies, msgs = [], []

    def on_message(msg, data):
        if msg.get("type") == "send":
            p = msg["payload"]
            msgs.append(p)
            if isinstance(p, dict) and p.get("kind") == "tally":
                tallies.append(p["counts"])
    script.on("message", on_message)
    script.load()
    time.sleep(5)
    before = dict(tallies[-1]) if tallies else {}
    print("tally-before:", before)

    root = ua.ControlFromHandle(hwnd)
    if root is None:
        print("FATAL: ControlFromHandle None")
        try: host.kill()
        except Exception: pass
        return 1

    # 1) Snapshot before opening the Assistant.
    dump_summary(root, "BEFORE Assistant open")

    # 2) Invoke the Master Assistant button.
    ma_btns = find_all(root, lambda c: name_of(c).lower() == "master assistant" and type_of(c) == 50000)
    print(f"\nMaster Assistant buttons found: {len(ma_btns)}")
    if not ma_btns:
        # fall back: any control whose name contains 'master assistant'
        ma_btns = find_all(root, lambda c: "master assistant" in name_of(c).lower())
        print(f"fallback Master Assistant controls: {len(ma_btns)}")
    opened = False
    for btn in ma_btns:
        print(f"  invoking Master Assistant ({name_of(btn)!r})")
        if invoke(btn):
            opened = True
            time.sleep(2.5)
            break
        else:
            # No invoke pattern -> it may be a toggle; try TogglePattern / SelectionItem.
            try:
                tp = btn.GetTogglePattern()
                if tp:
                    tp.Toggle(); opened = True; time.sleep(2.5); break
            except Exception:
                pass

    time.sleep(2.0)
    summary = dump_summary(root, "AFTER Master Assistant invoke")

    # 3) Invoke any analysis-start candidate that appeared.
    fired_any = False
    # Re-scan fresh each iteration (the view populates asynchronously).
    for attempt in range(3):
        fresh = dump_summary(root, f"START scan #{attempt+1}")
        if not fresh["hits"]:
            time.sleep(2.0)
            continue
        for _d, _ct, nm, _rect in fresh["hits"]:
            cands = find_all(root, lambda c, want=nm: name_of(c) == want and type_of(c) == 50000)
            for c in cands:
                print(f"  invoking start candidate {nm!r}")
                if invoke(c):
                    fired_any = True
                    time.sleep(3.0)
                    break
        time.sleep(3.0)

    # 4) Observe the pipeline.
    print("\nwaiting for analysis pipeline (~25s)...")
    time.sleep(25)
    after = dict(tallies[-1]) if tallies else {}
    delta = {kk: after.get(kk, 0) - before.get(kk, 0) for kk in set(before) | set(after)} if before else {}
    print("\ntally-after:", after)
    print("DELTA:", delta)
    pipeline_fired = delta.get("pipeline_root", 0) > 0 or delta.get("body", 0) > 0
    if pipeline_fired:
        print(">>> ANALYSIS PIPELINE FIRED — headless(no-human) trigger SUCCEEDED <<<")
    else:
        print(">>> pipeline_root/body delta = 0; analysis did NOT fire this run <<<")
        print(f"opened={opened} fired_any_start={fired_any}")

    # Save a small evidence blob next to the other captures.
    out_dir = ROOT / "tools" / "live_captures"
    out_dir.mkdir(parents=True, exist_ok=True)
    import json
    evidence = {
        "tool": "run_ozone_invoke_trigger.py",
        "hwnd": hex(hwnd),
        "pid": host.pid,
        "opened_assistant": opened,
        "invoked_start": fired_any,
        "tally_before": before,
        "tally_after": after,
        "delta": delta,
        "pipeline_fired": pipeline_fired,
        "post_named_buttons": summary["named"],
    }
    (out_dir / "invoke_trigger_evidence.json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
    print(f"\nevidence -> {out_dir/'invoke_trigger_evidence.json'}")

    try: script.unload()
    except Exception: pass
    session.detach()
    try: host.kill()
    except Exception: pass
    return 0 if pipeline_fired else 2


if __name__ == "__main__":
    sys.exit(main())
