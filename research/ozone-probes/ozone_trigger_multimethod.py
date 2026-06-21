#!/usr/bin/env python3
"""Decisive Master-Assistant-open test: try EVERY activation method on the
Master Assistant button, and after each, (a) capture a screenshot and (b)
re-walk the tree fresh to see if the Assistant View / Play control appeared.
Methods tried, in order, until one opens the view:
  1. UIA InvokePattern.Invoke
  2. LegacyIAccessible.DoDefaultAction
  3. SetFocus + Invoke
  4. Real foreground mouse click (mouse_event)
  5. Keyboard SPACE on the focused control
  6. Keyboard ENTER
We screenshot after each so a human (or vision) can SEE whether the Assistant
panel rendered, independent of the accessibility tree.
"""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path

import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
SHOT_DIR = ROOT / "tools" / "live_captures" / "shots"
SHOT_DIR.mkdir(parents=True, exist_ok=True)

u = ctypes.windll.user32
k = ctypes.windll.kernel32
MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x0002, 0x0004
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
VK_SPACE, VK_RETURN = 0x20, 0x0D
WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x0201, 0x0202, 0x0001


def foreground(hwnd):
    fg = u.GetForegroundWindow()
    tfg = u.GetWindowThreadProcessId(fg, None); me = k.GetCurrentThreadId()
    u.AttachThreadInput(me, tfg, True)
    try:
        u.ShowWindow(hwnd, 9); u.SetForegroundWindow(hwnd)
    finally:
        u.AttachThreadInput(me, tfg, False)


def real_click(x, y):
    u.SetCursorPos(x, y); time.sleep(0.1)
    u.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0); time.sleep(0.05)
    u.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); time.sleep(0.08)


def screenshot(hwnd, name):
    """Capture the window to a BMP via PrintWindow (works offscreen too)."""
    try:
        import win32gui, win32ui, win32con
        l, t, r, b = win32gui.GetWindowRect(hwnd)
        w, h = r - l, b - t
        hdc = win32gui.GetWindowDC(hwnd)
        mfc = win32ui.CreateDCFromHandle(hdc)
        save = mfc.CreateCompatibleDC()
        bmp = win32ui.CreateBitmap()
        bmp.CreateCompatibleBitmap(mfc, w, h)
        save.SelectObject(bmp)
        # PW_RENDERFULLCONTENT = 2 (Win 8.1+)
        ctypes.windll.user32.PrintWindow(hwnd, save.GetSafeHdc(), 2)
        path = str(SHOT_DIR / name)
        bmp.SaveBitmapFile(save, path)
        save.DeleteDC(); mfc.DeleteDC(); win32gui.ReleaseDC(hwnd, hdc)
        win32gui.DeleteObject(bmp.GetHandle())
        return path
    except Exception as e:
        return f"shot-failed: {e}"


def walk(c, d=0, acc=None, maxd=18):
    if acc is None: acc = []
    if d > maxd: return acc
    try: kids = c.GetChildren()
    except Exception: kids = []
    for x in kids:
        try:
            acc.append((d, x.ControlType, x.Name or "", x.BoundingRectangle))
        except Exception:
            continue
        walk(x, d + 1, acc, maxd)
    return acc


def assistant_opened(items):
    hints = ("play", "learn", "listen", "start", "begin", "analy", "vibe",
             "target", "genre", "open assistant")
    return [it for it in items
            if any(h in (it[2] or "").lower() for h in hints)
            and "master assistant" not in (it[2] or "").lower()]


def wait_hwnd(host, timeout=25.0):
    hwnd, deadline = None, time.time() + timeout
    while time.time() < deadline and hwnd is None:
        line = host.stderr.readline()
        if not line:
            if host.poll() is not None: break
            continue
        m = re.search(r"OZONE_EDITOR_HWND=0x([0-9a-fA-F]+)", line)
        if m: hwnd = int(m.group(1), 16)
    return hwnd


def find_ma(root):
    found = []
    def rec(c):
        try: kids = c.GetChildren()
        except Exception: kids = []
        for x in kids:
            try:
                if (x.Name or "").strip().lower() == "master assistant" and x.ControlType == 50000:
                    found.append(x)
            except Exception: pass
            rec(x)
            if found: return
    rec(root)
    return found[0] if found else None


def count_named(items):
    return sorted({nm for (d, ct, nm, r) in items if nm and ct == 50000})


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "200"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = wait_hwnd(host)
    if hwnd is None:
        print("no HWND")
        try: host.kill()
        except Exception: pass
        return 1
    print(f"HWND=0x{hwnd:x}")
    u.MoveWindow(hwnd, 30, 30, 1160, 600, True)
    foreground(hwnd); time.sleep(2.0)

    root = ua.ControlFromHandle(hwnd)
    shot0 = screenshot(hwnd, "00_baseline.bmp")
    items0 = walk(root)
    print(f"baseline shot: {shot0}")
    print(f"baseline named buttons: {count_named(items0)}")

    ma = find_ma(root)
    if ma is None:
        print("FATAL: no Master Assistant button")
        try: host.kill()
        except Exception: pass
        return 1
    rect = ma.BoundingRectangle
    cx, cy = (rect.left + rect.right) // 2, (rect.top + rect.bottom) // 2
    print(f"Master Assistant at ({cx},{cy})")

    methods = [
        ("01_invoke", lambda: ma.GetInvokePattern().Invoke() if ma.GetInvokePattern() else None),
        ("02_doDefaultAction", lambda: ma.GetLegacyIAccessiblePattern().DoDefaultAction() if ma.GetLegacyIAccessiblePattern() else None),
        ("03_setfocus_invoke", lambda: (ma.SetFocus(), time.sleep(0.3), ma.GetInvokePattern().Invoke() if ma.GetInvokePattern() else None)),
        ("04_real_click", lambda: real_click(cx, cy)),
        ("05_key_space", lambda: (ma.SetFocus(), time.sleep(0.3), u.PostMessageW(hwnd, WM_KEYDOWN, VK_SPACE, 0), time.sleep(0.1), u.PostMessageW(hwnd, WM_KEYUP, VK_SPACE, 0))),
        ("06_key_enter", lambda: (u.PostMessageW(hwnd, WM_KEYDOWN, VK_RETURN, 0), time.sleep(0.1), u.PostMessageW(hwnd, WM_KEYUP, VK_RETURN, 0))),
        ("07_postmsg_click", lambda: (u.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, ((cy - 30) << 16) | ((cx - 30) & 0xffff)), time.sleep(0.07), u.PostMessageW(hwnd, WM_LBUTTONUP, 0, ((cy - 30) << 16) | ((cx - 30) & 0xffff)))),
    ]

    success = None
    for name, fn in methods:
        # Re-fetch MA fresh each time (cache may be stale)
        ma = find_ma(root) or ma
        try:
            fn()
            print(f"\n== {name} executed ==")
        except Exception as e:
            print(f"\n== {name} FAILED: {e} ==")
            continue
        time.sleep(3.5)
        shot = screenshot(hwnd, f"{name}.bmp")
        items = walk(root)
        named = count_named(items)
        opened = assistant_opened(items)
        new_btns = sorted(set(named) - set(count_named(items0)))
        print(f"  shot: {shot}")
        print(f"  new buttons vs baseline: {new_btns}")
        print(f"  assistant-open candidates: {opened}")
        if opened or new_btns:
            print(f"  >>> {name} OPENED SOMETHING <<<")
            success = name
            break

    if not success:
        print("\n>>> No activation method opened the Assistant View <<<")
        print("Baseline buttons never changed across all 7 methods.")

    try: host.kill()
    except Exception: pass
    return 0 if success else 2


if __name__ == "__main__":
    raise SystemExit(main())
