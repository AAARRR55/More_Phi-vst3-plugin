#!/usr/bin/env python3
"""Diff probe: locate the REAL native Ozone child window, click the Master
Assistant button through THAT window's message queue (not the outer JUCE
HWND), and diff the full accessible control set (named + unnamed, with rects)
before vs after. The goal is to detect whether the Assistant View actually
swaps in (new controls / Play button), and if so where the analysis-start
control lands so a subsequent Invoke() can fire it headlessly.
"""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path

import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
u = ctypes.windll.user32

WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x0201, 0x0202, 0x0001

EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)


def all_windows_for_pid(pid):
    found = []

    def cb(hwnd, _):
        if not u.IsWindowVisible(hwnd):
            return True
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            buf = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(hwnd, buf, 256)
            cls = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(hwnd, cls, 256)
            r = wintypes.RECT()
            u.GetWindowRect(hwnd, ctypes.byref(r))
            found.append((hwnd, buf.value, cls.value, (r.left, r.top, r.right, r.bottom)))
        return True

    def cb_all(hwnd, _):
        # include invisible too (Assistant overlay may start hidden)
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        if p.value == pid:
            buf = ctypes.create_unicode_buffer(256)
            u.GetWindowTextW(hwnd, buf, 256)
            cls = ctypes.create_unicode_buffer(256)
            u.GetClassNameW(hwnd, cls, 256)
            r = wintypes.RECT()
            u.GetWindowRect(hwnd, ctypes.byref(r))
            found.append((hwnd, buf.value, cls.value, (r.left, r.top, r.right, r.bottom), u.IsWindowVisible(hwnd)))
        return True

    u.EnumWindows(EnumWindowsProc(cb_all), 0)
    return found


def walk_all(c, d=0, acc=None, maxd=16):
    if acc is None:
        acc = []
    if d > maxd:
        return acc
    try:
        kids = c.GetChildren()
    except Exception:
        kids = []
    for x in kids:
        try:
            nm = x.Name or ""
            ct = x.ControlType
            r = x.BoundingRectangle
            rect = (r.left, r.top, r.right, r.bottom)
            w_ = r.right - r.left
            h_ = r.bottom - r.top
        except Exception:
            continue
        acc.append((d, ct, nm, rect, (w_, h_)))
        walk_all(x, d + 1, acc, maxd)
    return acc


def click_at(target_hwnd, screen_x, screen_y):
    pt = wintypes.POINT(screen_x, screen_y)
    u.ScreenToClient(target_hwnd, ctypes.byref(pt))
    lp = (pt.y << 16) | (pt.x & 0xFFFF)
    u.PostMessageW(target_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp)
    time.sleep(0.07)
    u.PostMessageW(target_hwnd, WM_LBUTTONUP, 0, lp)
    return (target_hwnd, pt.x, pt.y)


def find_native_ozone_child(parent_hwnd):
    """Find Ozone's own win32 child window by class prefix."""
    target = []

    def cb(hwnd, _):
        cls = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(hwnd, cls, 256)
        if "iZ_OZONEPRO" in cls.value:
            target.append((hwnd, cls.value))
        child = u.FindWindowExW(hwnd, 0, None, None)
        return True

    u.EnumChildWindows(parent_hwnd, EnumWindowsProc(cb), 0)
    return target


def wait_hwnd(host, timeout=25.0):
    hwnd, deadline = None, time.time() + timeout
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
    env["OZONE_HOST_SECONDS"] = "130"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = wait_hwnd(host)
    if hwnd is None:
        print("FATAL: no editor HWND")
        try: host.kill()
        except Exception: pass
        return 1
    print(f"editor HWND = 0x{hwnd:x}")

    u.MoveWindow(hwnd, 40, 40, 1160, 600, True)
    time.sleep(1.5)

    print("\n=== ALL windows for pid (visible+hidden) ===")
    wins = all_windows_for_pid(host.pid)
    for h, title, cls, r, vis in wins:
        print(f"  HWND=0x{h:x} vis={vis} title={title!r} class={cls!r} rect={r}")

    children = find_native_ozone_child(hwnd)
    print(f"\nNative Ozone child windows: {children}")
    if not children:
        print("No iZ_OZONEPRO child window — using editor HWND as click target.")
        click_target = hwnd
    else:
        click_target = children[-1][0]  # deepest / most-specific win32 window
    print(f"click target HWND = 0x{click_target:x}")

    root = ua.ControlFromHandle(hwnd)

    # BEFORE snapshot
    before = walk_all(root)
    print(f"\nBEFORE: {len(before)} controls")

    # Locate Master Assistant button (screen coords)
    ma = [(d, ct, nm, rect) for (d, ct, nm, rect, wh) in before
          if nm.strip().lower() == "master assistant" and ct == 50000]
    if not ma:
        print("FATAL: no Master Assistant button found")
        try: host.kill()
        except Exception: pass
        return 1
    d0, ct0, nm0, (l, t, rr, b) = ma[0]
    cx, cy = (l + rr) // 2, (t + b) // 2
    print(f"Master Assistant button screen=({cx},{cy}) size={rr-l}x{b-t}")

    print(f"\n>> PostMessage click to child 0x{click_target:x} at ({cx},{cy})")
    print("  result:", click_at(click_target, cx, cy))
    time.sleep(4.0)

    # AFTER snapshot
    after = walk_all(root)
    print(f"AFTER: {len(after)} controls")

    def sig(item):
        d, ct, nm, rect, wh = item
        return (ct, nm, rect, wh)

    bset = {sig(x) for x in before}
    aset = {sig(x) for x in after}
    added = [x for x in after if sig(x) not in bset]
    removed = [x for x in before if sig(x) not in aset]
    print(f"\n=== DIFF: {len(added)} added, {len(removed)} removed ===")
    print("--- ADDED controls ---")
    for d, ct, nm, rect, wh in added:
        print(f"  {'  '*d}{ct} name={nm!r} rect={rect} size={wh}")
    print("--- REMOVED controls ---")
    for d, ct, nm, rect, wh in removed:
        print(f"  {'  '*d}{ct} name={nm!r} rect={rect} size={wh}")

    # Re-scan windows too (overlay may appear)
    print("\n=== windows AFTER click ===")
    for h, title, cls, r, vis in all_windows_for_pid(host.pid):
        print(f"  HWND=0x{h:x} vis={vis} title={title!r} class={cls!r} rect={r}")

    try: host.kill()
    except Exception: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
