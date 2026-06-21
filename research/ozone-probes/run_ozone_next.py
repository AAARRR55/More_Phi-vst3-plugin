#!/usr/bin/env python3
"""Per user hint: the Master Assistant is a multi-step wizard. Click 'Master
Assistant', then 'Next' (repeat), and observe the analysis pipeline via Frida.
Real, foreground-held clicks that actually land on the Ozone window."""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path
import frida, uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")
u = ctypes.windll.user32
k = ctypes.windll.kernel32
DOWN, UP = 0x0002, 0x0004


def fg_click(hwnd, control):
    r = control.BoundingRectangle
    x = (r.left + r.right) // 2; y = (r.top + r.bottom) // 2
    fg = u.GetForegroundWindow(); attached = False
    if fg != hwnd:
        tfg = u.GetWindowThreadProcessId(fg, None); tme = k.GetCurrentThreadId()
        u.AttachThreadInput(tme, tfg, True); attached = True
        u.ShowWindow(hwnd, 9); u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd); time.sleep(0.05)
    u.SetCursorPos(x, y); time.sleep(0.05)
    actual = u.GetForegroundWindow()
    u.mouse_event(DOWN, 0, 0, 0, 0); time.sleep(0.05); u.mouse_event(UP, 0, 0, 0, 0)
    return (actual == hwnd, x, y)


def find_all(root, name_substr):
    out = []
    def walk(c):
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                if name_substr in (x.Name or "").lower(): out.append(x)
            except: pass
            walk(x)
    walk(root); return out


def named_buttons(root):
    acc = []
    def walk(c, d=0, md=14):
        if d > md: return
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                nm = (x.Name or "").strip()
                if nm and x.ControlType == 50000:
                    acc.append(nm)
            except: pass
            walk(x, d + 1, md)
    walk(root); return acc


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "130"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True, bufsize=1)
    hwnd = None; t = time.time() + 25
    while time.time() < t and hwnd is None:
        line = host.stderr.readline()
        if not line:
            if host.poll() is not None: break
            continue
        m = re.search(r"OZONE_EDITOR_HWND=0x([0-9a-fA-F]+)", line)
        if m: hwnd = int(m.group(1), 16)
    if hwnd is None: print("no HWND"); host.kill(); return 1
    print(f"HWND=0x{hwnd:x}")
    u.MoveWindow(hwnd, 40, 40, 1140, 580, True); time.sleep(1)

    sess = frida.get_local_device().attach(host.pid)
    sc = sess.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []
    sc.on("message", lambda m, d: tallies.append(m["payload"]["counts"])
          if m.get("type") == "send" and m["payload"].get("kind") == "tally" else None)
    sc.load(); time.sleep(4); before = dict(tallies[-1]) if tallies else {}

    root = ua.ControlFromHandle(hwnd)

    # Step 1: Master Assistant
    ma = find_all(root, "master assistant")
    if ma:
        ok, x, y = fg_click(hwnd, ma[0]); print(f"Master Assistant click landed={ok} @({x},{y})")
    time.sleep(3.0)
    print("buttons after MA:", sorted(set(named_buttons(root))))

    # Dump ALL controls (incl. unnamed) with rects to see the Assistant panel structure
    root = ua.ControlFromHandle(hwnd)
    allc = []
    def walk(c, d=0, md=14):
        if d > md: return
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                r = x.BoundingRectangle; w = r.right - r.left; h = r.bottom - r.top
                allc.append((d, x.ControlType, x.Name or "", (r.left, r.top, w, h)))
            except: continue
            walk(x, d + 1, md)
    walk(root)
    print(f"\n=== full tree after MA click: {len(allc)} controls. Named + large + buttons: ===")
    for d, ct, nm, (l, t, w, h) in allc:
        if nm.strip() or (ct == 50000) or (w >= 60 and h >= 60):
            print(f"{'  '*d}{ct} name={nm!r} @({l},{t}) {w}x{h}")

    # Steps 2..N: click 'Next' (and other wizard words) repeatedly
    wiz = ("next", "continue", "play", "learn", "listen", "start", "begin", "open", "analyze", "ok", "done", "finish", "apply")
    for step in range(8):
        root = ua.ControlFromHandle(hwnd)
        cands = []
        for w in wiz:
            for c in find_all(root, w):
                try:
                    nm = (c.Name or "").lower()
                    if nm != "master assistant":
                        cands.append((w, c, nm))
                except: pass
        # de-dup by control identity
        seen = set(); uniq = []
        for w, c, nm in cands:
            try:
                r = c.BoundingRectangle
                key = (r.left, r.top, r.right, r.bottom, nm)
            except:
                key = (0, 0, 0, 0, nm)
            if key in seen: continue
            seen.add(key); uniq.append((w, c, nm))
        if not uniq:
            print(f"step {step}: no wizard button found; stopping.")
            break
        # pick the best candidate (prefer 'next', then 'play'/'learn'/'listen'/'start')
        priority = {"next": 0, "continue": 1, "play": 2, "learn": 2, "listen": 2, "start": 2, "begin": 2}
        uniq.sort(key=lambda t: priority.get(t[0], 5))
        w, c, nm = uniq[0]
        ok, x, y = fg_click(hwnd, c)
        print(f"step {step}: clicked {nm!r} (word={w!r}) landed={ok} @({x},{y})")
        print(f"   buttons now: {sorted(set(named_buttons(ua.ControlFromHandle(hwnd))))[:20]}")
        time.sleep(4.0)

    time.sleep(14)
    after = tallies[-1] if tallies else {}
    delta = {kk: after.get(kk, 0) - before.get(kk, 0) for kk in set(before) | set(after)} if before else {}
    print("\nDELTA:", delta)
    if delta.get("pipeline_root", 0) > 0 or delta.get("body", 0) > 0:
        print(">>> ANALYSIS PIPELINE FIRED — headless trigger succeeded <<<")
    try: sc.unload()
    except: pass
    sess.detach()
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
