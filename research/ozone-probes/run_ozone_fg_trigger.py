#!/usr/bin/env python3
"""Foreground the offscreen Ozone editor (AttachThreadInput + SetForegroundWindow,
no onscreen move), then PostMessage-click 'Master Assistant' to open the Assistant
View, find the Play/Learn button, click it, and observe the analysis pipeline."""
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
WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x0201, 0x0202, 0x0001


def foreground(hwnd):
    fg = u.GetForegroundWindow()
    tfg = u.GetWindowThreadProcessId(fg, None)
    tme = k.GetCurrentThreadId()
    u.AttachThreadInput(tme, tfg, True)
    u.ShowWindow(hwnd, 9)  # SW_RESTORE
    u.SetForegroundWindow(hwnd)
    u.AttachThreadInput(tme, tfg, False)


def owning_hwnd(control):
    c = control
    for _ in range(24):
        try:
            h = c.NativeWindowHandle
            if h:
                return h
        except Exception:
            pass
        try:
            p = c.GetParent()
        except Exception:
            p = None
        if p is None or p == c:
            break
        c = p
    return None


def click(top, control):
    hwnd = owning_hwnd(control) or top
    r = control.BoundingRectangle
    cx = (r.left + r.right) // 2; cy = (r.top + r.bottom) // 2
    pt = wintypes.POINT(cx, cy); u.ScreenToClient(hwnd, ctypes.byref(pt))
    lp = (pt.y << 16) | (pt.x & 0xffff)
    u.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp); time.sleep(0.06)
    u.PostMessageW(hwnd, WM_LBUTTONUP, 0, lp)
    return (hex(hwnd), pt.x, pt.y)


def find_btn(root, name):
    found = []
    def walk(c):
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                if (x.Name or "").strip().lower() == name: found.append(x)
            except: pass
            walk(x)
    walk(root); return found


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "100"
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

    sess = frida.get_local_device().attach(host.pid)
    sc = sess.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []
    sc.on("message", lambda m, d: tallies.append(m["payload"]["counts"])
          if m.get("type") == "send" and m["payload"].get("kind") == "tally" else None)
    sc.load()
    time.sleep(4); before = dict(tallies[-1]) if tallies else {}
    print("tally-before:", before)

    root = ua.ControlFromHandle(hwnd)
    foreground(hwnd); time.sleep(0.5)

    ma = find_btn(root, "master assistant")
    if ma:
        print("click Master Assistant:", click(hwnd, ma[0]))
    time.sleep(3.5)

    # Re-scan: did the Assistant View open? look for Play/Learn + dump view-ish controls
    post = []
    def walk(c, d=0, md=12):
        if d > md: return
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try: post.append((d, x.ControlType, x.Name or "", x.BoundingRectangle))
            except: continue
            walk(x, d + 1, md)
    walk(root)
    hints = ("play", "learn", "listen", "start", "begin", "vibe", "target", "genre", "open", "analy")
    cands = [(d, ct, nm, r) for (d, ct, nm, r) in post if any(h in nm.lower() for h in hints) and nm.strip().lower() != "master assistant"]
    print(f"\npost-open hint controls ({len(cands)}):")
    for d, ct, nm, r in cands:
        print(f"{'  '*d}{ct} name={nm!r} rect=({r.left},{r.top},{r.right},{r.bottom})")
    # also dump all named buttons
    named_btns = [(d, ct, nm) for (d, ct, nm, r) in post if ct == 50000 and nm.strip()]
    print(f"named buttons ({len(named_btns)}):", [nm for (d, ct, nm) in named_btns])

    for d, ct, nm, r in cands:
        b = find_btn(root, nm.lower())
        if b:
            print(f"click {nm!r}:", click(hwnd, b[0])); time.sleep(3)

    time.sleep(18)
    after = tallies[-1] if tallies else {}
    delta = {kk: after.get(kk, 0) - before.get(kk, 0) for kk in set(before) | set(after)} if before else {}
    print("\ntally-after:", after)
    print("DELTA:", delta)
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
