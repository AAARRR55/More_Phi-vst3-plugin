#!/usr/bin/env python3
"""Decisive test: does mouse input reach the Master Assistant wizard at all?
  1. Open wizard via Invoke (works).
  2. Foreground the window.
  3. Screenshot (open state).
  4. Click the wizard Close-X via true SendInput at its screen coord.
  5. Screenshot again + re-scan tree.
If the wizard CLOSES (tree returns to toolbar, or pixels change significantly),
mouse input reaches the wizard -> Next/Play just need correct coords.
If nothing changes -> the wizard is input-immune in this headless host.
"""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path
import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
u = ctypes.windll.user32
k = ctypes.windll.kernel32

# SendInput structures
INPUT_MOUSE = 0
MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_ABSOLUTE = 0x8000


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                ("mouseData", ctypes.c_ulong), ("dwFlags", ctypes.c_ulong),
                ("time", ctypes.c_ulong), ("dwExtraInfo", ctypes.c_void_p)]


class INPUT(ctypes.Structure):
    class _INPUT(ctypes.Union):
        _fields_ = [("mi", MOUSEINPUT)]
    _anonymous_ = ("_input",)
    _fields_ = [("type", ctypes.c_ulong), ("_input", _INPUT)]


def sendinput_click(screen_x, screen_y):
    # map screen coords to absolute (0..65535)
    cx = ctypes.windll.user32.GetSystemMetrics(78)  # SM_CXSCREEN
    cy = ctypes.windll.user32.GetSystemMetrics(79)  # SM_CYSCREEN
    dx = int(screen_x * 65535 / cx)
    dy = int(screen_y * 65535 / cy)
    u.SetCursorPos(screen_x, screen_y)
    time.sleep(0.12)
    down = INPUT(type=INPUT_MOUSE, _input=INPUT._INPUT(mi=MOUSEINPUT(
        dx=dx, dy=dy, mouseData=0,
        dwFlags=MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN, time=0, dwExtraInfo=None)))
    up = INPUT(type=INPUT_MOUSE, _input=INPUT._INPUT(mi=MOUSEINPUT(
        dx=dx, dy=dy, mouseData=0,
        dwFlags=MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP, time=0, dwExtraInfo=None)))
    u.SendInput(1, ctypes.byref(down), ctypes.sizeof(INPUT))
    time.sleep(0.08)
    u.SendInput(1, ctypes.byref(up), ctypes.sizeof(INPUT))
    time.sleep(0.15)


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


def foreground(hwnd):
    fg = u.GetForegroundWindow()
    tfg = u.GetWindowThreadProcessId(fg, None)
    me = k.GetCurrentThreadId()
    u.AttachThreadInput(me, tfg, True)
    try:
        u.ShowWindow(hwnd, 9)
        u.SetForegroundWindow(hwnd)
    finally:
        u.AttachThreadInput(me, tfg, False)
    time.sleep(0.5)


def shot(hwnd, name):
    import win32gui, win32ui
    l, t, r, b = win32gui.GetWindowRect(hwnd)
    w, h = r - l, b - t
    hdc = win32gui.GetWindowDC(hwnd)
    mfc = win32ui.CreateDCFromHandle(hdc)
    save = mfc.CreateCompatibleDC()
    bmp = win32ui.CreateBitmap()
    bmp.CreateCompatibleBitmap(mfc, w, h)
    save.SelectObject(bmp)
    u.PrintWindow(hwnd, save.GetSafeHdc(), 2)
    out = str(ROOT / "tools" / "live_captures" / "shots" / name)
    bmp.SaveBitmapFile(save, out)
    save.DeleteDC(); mfc.DeleteDC(); win32gui.ReleaseDC(hwnd, hdc)
    win32gui.DeleteObject(bmp.GetHandle())
    return out


def brightness(path):
    from PIL import Image
    im = Image.open(path).convert('RGB')
    px = im.load(); w, h = im.size
    br = 0; n = 0
    for y in range(0, h, 4):
        for x in range(0, w, 4):
            c = px[x, y]; br += (c[0] + c[1] + c[2]) / 3; n += 1
    return round(br / n, 1), im.size


def diff_pct(a_path, b_path):
    from PIL import Image
    a = Image.open(a_path).convert('RGB'); b = Image.open(b_path).convert('RGB')
    ax = a.load(); bx = b.load(); w, h = a.size
    d = 0; t = 0
    for y in range(0, h, 3):
        for x in range(0, w, 3):
            ca = ax[x, y]; cb = bx[x, y]
            if abs(ca[0]-cb[0])+abs(ca[1]-cb[1])+abs(ca[2]-cb[2]) > 24:
                d += 1
            t += 1
    return round(100*d/t, 2)


def find_first(root, name_lower):
    out = []
    def rec(c):
        if out: return
        try: kids = c.GetChildren()
        except Exception: kids = []
        for x in kids:
            try:
                if (x.Name or "").strip().lower() == name_lower: out.append(x)
            except Exception: pass
            rec(x)
            if out: return
    rec(root)
    return out[0] if out else None


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "90"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = wait_hwnd(host)
    if hwnd is None:
        print("no HWND")
        try: host.kill()
        except Exception: pass
        return 1
    print(f"HWND=0x{hwnd:x}")
    u.MoveWindow(hwnd, 40, 40, 1160, 600, True)
    u.ShowWindow(hwnd, 5)
    time.sleep(3.0)
    foreground(hwnd)

    root = ua.ControlFromHandle(hwnd)
    ma = find_first(root, "master assistant")
    ma.GetInvokePattern().Invoke()
    print("wizard opened via Invoke")
    time.sleep(4.0)
    p_open = shot(hwnd, "mt_open.bmp")
    print(f"open shot bright={brightness(p_open)}")

    # Vision: Close-X bbox in 1413x691 capture = (754,69)-(772,109) -> center (763,89)
    # Window placed at (40,40) full-rect. So screen coord = (40+763, 40+89) = (803,129).
    import win32gui
    wl, wt, wr, wb = win32gui.GetWindowRect(hwnd)
    ww, wh = wr - wl, wb - wt
    close_x = wl + int(0.540 * ww)   # 763/1413
    close_y = wt + int(0.129 * wh)   # 89/691
    print(f"clicking Close-X at screen ({close_x},{close_y}) [win {ww}x{wh}]")
    sendinput_click(close_x, close_y)
    time.sleep(3.0)
    p_close = shot(hwnd, "mt_after_close.bmp")
    print(f"after-close shot bright={brightness(p_close)}")
    print(f"open->after-close diff = {diff_pct(p_open, p_close)}%")
    if diff_pct(p_open, p_close) > 2.0:
        print(">>> WIZARD CHANGED -> mouse input reaches the wizard <<<")
    else:
        print(">>> wizard unchanged -> mouse input does NOT reach it <<<")

    try: host.kill()
    except Exception: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
