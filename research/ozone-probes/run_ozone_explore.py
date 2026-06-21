#!/usr/bin/env python3
"""Comprehensive exploration: after clicking 'Master Assistant', enumerate ALL
windows owned by the host process (to catch popup Assistant views) and dump the
full control tree of each (including unnamed controls, with rects), to locate
the analysis-start (Play/Learn) control by name, type, or position/size."""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path
import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
user32 = ctypes.windll.user32
WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x0201, 0x0202, 0x0001

EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def all_windows_for_pid(pid):
    found = []
    def cb(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            p = wintypes.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
            if p.value == pid:
                ln = wintypes.DWORD()
                user32.GetWindowTextLengthW(hwnd)
                buf = ctypes.create_unicode_buffer(256)
                user32.GetWindowTextW(hwnd, buf, 256)
                cls = ctypes.create_unicode_buffer(256)
                user32.GetClassNameW(hwnd, cls, 256)
                r = wintypes.RECT(); user32.GetWindowRect(hwnd, ctypes.byref(r))
                found.append((hwnd, buf.value, cls.value, (r.left, r.top, r.right, r.bottom)))
        return True
    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return found


def postmsg_click(top, control):
    r = control.BoundingRectangle
    cx = (r.left + r.right) // 2; cy = (r.top + r.bottom) // 2
    pt = wintypes.POINT(cx, cy); user32.ScreenToClient(top, ctypes.byref(pt))
    lp = (pt.y << 16) | (pt.x & 0xffff)
    user32.PostMessageW(top, WM_LBUTTONDOWN, MK_LBUTTON, lp); time.sleep(0.05)
    user32.PostMessageW(top, WM_LBUTTONUP, 0, lp)
    return (pt.x, pt.y)


def walk_all(c, d, acc, maxd=12):
    if d > maxd: return
    try: ch = c.GetChildren()
    except: ch = []
    for x in ch:
        try:
            nm = x.Name or ""; ct = x.ControlType; r = x.BoundingRectangle
            w = r.right - r.left; h = r.bottom - r.top
        except: continue
        acc.append((d, ct, nm, (r.left, r.top, w, h)))
        walk_all(x, d + 1, acc, maxd)


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "120"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE, env=env, text=True, bufsize=1)
    pid = host.pid
    hwnd = None; t = time.time() + 25
    while time.time() < t and hwnd is None:
        line = host.stderr.readline()
        if not line:
            if host.poll() is not None: break
            continue
        m = re.search(r"OZONE_EDITOR_HWND=0x([0-9a-fA-F]+)", line)
        if m: hwnd = int(m.group(1), 16)
    if hwnd is None: print("no HWND"); host.kill(); return 1
    print(f"host pid={pid} editor HWND=0x{hwnd:x}")

    root = ua.ControlFromHandle(hwnd)

    # Click Master Assistant
    btn = None
    def find_ma(c):
        nonlocal btn
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                if (x.Name or "").strip().lower() == "master assistant": btn = x; return
            except: pass
            find_ma(x)
            if btn: return
    find_ma(root)
    if btn:
        print("Master Assistant rect:", btn.BoundingRectangle)
        print("click:", postmsg_click(hwnd, btn))
    time.sleep(3.5)

    # All process windows (catch popups)
    print("\n=== ALL visible windows for host pid ===")
    for h, title, cls, r in all_windows_for_pid(pid):
        print(f"  HWND=0x{h:x} title={title!r} class={cls!r} rect={r}")

    # Full control tree of the editor (every control, incl. unnamed), large controls first
    acc = []; walk_all(root, 0, acc)
    print(f"\n=== editor tree: {len(acc)} controls. Large (>80x80) + buttons: ===")
    big = [(d, ct, nm, (l, t, w, h)) for (d, ct, nm, (l, t, w, h)) in acc if (w >= 80 and h >= 80) or ct == 50000]
    for (d, ct, nm, (l, t, w, h)) in big:
        print(f"{'  '*d}{ct} name={nm!r} @({l},{t}) {w}x{h}")

    print("\n=== controls whose name hints at analysis/start ===")
    hints = ("play", "learn", "listen", "start", "analyze", "begin", "vibe", "target",
             "open", "master assistant", "genre", "reference")
    for (d, ct, nm, (l, t, w, h)) in acc:
        if any(k in (nm or "").lower() for k in hints):
            print(f"{'  '*d}{ct} name={nm!r} @({l},{t}) {w}x{h}")

    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
