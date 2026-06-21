#!/usr/bin/env python3
"""Robust: tall window + foreground + BitBlt screenshot via ctypes (no PIL/PS).
Click Master Assistant, capture the Ozone window to a PNG-equivalent we can read.
Falls back to PowerShell CopyFromScreen with a backslash path + error capture."""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path
import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
SHOTS = ROOT / "tools" / "live_captures" / "shots"
SHOTS.mkdir(parents=True, exist_ok=True)
u = ctypes.windll.user32
k = ctypes.windll.kernel32
DOWN, UP = 0x0002, 0x0004


def fg_click(hwnd, control):
    r = control.BoundingRectangle
    x = (r.left + r.right) // 2; y = (r.top + r.bottom) // 2
    fg = u.GetForegroundWindow()
    if fg != hwnd:
        tfg = u.GetWindowThreadProcessId(fg, None); tme = k.GetCurrentThreadId()
        u.AttachThreadInput(tme, tfg, True)
        u.ShowWindow(hwnd, 9); u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd); time.sleep(0.05)
    u.SetCursorPos(x, y); time.sleep(0.05)
    a = u.GetForegroundWindow()
    u.mouse_event(DOWN, 0, 0, 0, 0); time.sleep(0.05); u.mouse_event(UP, 0, 0, 0, 0)
    return (a == hwnd, x, y)


def find(root, name):
    out = []
    def walk(c):
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                if name in (x.Name or "").lower(): out.append(x)
            except: pass
            walk(x)
    walk(root); return out


def shot_png(l, t, w, h, path_win):
    ps = ("Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
          "$b=New-Object System.Drawing.Bitmap %d,%d;"
          "$g=[System.Drawing.Graphics]::FromImage($b);"
          "$g.CopyFromScreen(%d,%d,0,0,(New-Object System.Drawing.Size(%d,%d)));"
          "$g.Dispose(); $b.Save('%s'); Write-Output $LASTEXITCODE") % (
          w, h, l, t, w, h, path_win)
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                       capture_output=True, text=True)
    return r.stdout + r.stderr


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "70"
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
    # Tall window so the full Assistant panel + Next button fit
    u.MoveWindow(hwnd, 20, 20, 1160, 860, True)
    # force foreground
    fg = u.GetForegroundWindow(); tfg = u.GetWindowThreadProcessId(fg, None); tme = k.GetCurrentThreadId()
    u.AttachThreadInput(tme, tfg, True); u.ShowWindow(hwnd, 9); u.SetForegroundWindow(hwnd); u.AttachThreadInput(tme, tfg, False)
    time.sleep(1.5)
    root = ua.ControlFromHandle(hwnd)

    base = str(SHOTS / "my_baseline.png")
    r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
    print("shot baseline:", shot_png(r.left, r.top, r.right - r.left, r.bottom - r.top, base)[:200])

    ma = find(root, "master assistant")
    if ma:
        ok, x, y = fg_click(hwnd, ma[0]); print(f"clicked MA landed={ok} @({x},{y})")
    time.sleep(4.0)
    r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
    ma_shot = str(SHOTS / "my_after_ma.png")
    print("shot after-MA:", shot_png(r.left, r.top, r.right - r.left, r.bottom - r.top, ma_shot)[:200])
    print(f"files: {[f.name for f in SHOTS.glob('my_*.png')]}")
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
