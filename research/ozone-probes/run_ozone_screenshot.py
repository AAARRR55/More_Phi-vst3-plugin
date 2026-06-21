#!/usr/bin/env python3
"""Click Master Assistant (foreground-held real click), then SCREENSHOT the Ozone
window to see whether the Assistant panel visually opened (it may be custom-drawn
with no accessible labels). Saves a PNG for inspection."""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path
import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
OUT = str(ROOT / "tools" / "live_captures" / "static" / "ozone_ma.png")
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
    actual = u.GetForegroundWindow()
    u.mouse_event(DOWN, 0, 0, 0, 0); time.sleep(0.05); u.mouse_event(UP, 0, 0, 0, 0)
    return (actual == hwnd, x, y)


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


def screenshot(l, t, w, h, path):
    ps = f'''
Add-Type -AssemblyName System.Windows.Forms,System.Drawing
$b = New-Object System.Drawing.Bitmap {w},{h}
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen({l},{t},0,0,(New-Object System.Drawing.Size({w},{h})))
$b.Save(r'{path}')
'''
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], check=False,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "60"
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
    u.MoveWindow(hwnd, 40, 40, 1140, 580, True); time.sleep(1.5)
    root = ua.ControlFromHandle(hwnd)

    # baseline screenshot
    r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
    screenshot(r.left, r.top, r.right - r.left, r.bottom - r.top,
               str(ROOT / "tools" / "live_captures" / "static" / "ozone_baseline.png"))
    print("baseline shot saved.")

    ma = find(root, "master assistant")
    if ma:
        ok, x, y = fg_click(hwnd, ma[0]); print(f"clicked MA landed={ok} @({x},{y})")
    time.sleep(3.5)
    r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
    screenshot(r.left, r.top, r.right - r.left, r.bottom - r.top, OUT)
    print(f"post-MA shot saved -> {OUT}")

    # double-click too
    if ma:
        fg_click(hwnd, ma[0]); time.sleep(0.2); fg_click(hwnd, ma[0])
    time.sleep(4)
    screenshot(r.left, r.top, r.right - r.left, r.bottom - r.top,
               str(ROOT / "tools" / "live_captures" / "static" / "ozone_ma_double.png"))
    print("double-click shot saved.")
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
