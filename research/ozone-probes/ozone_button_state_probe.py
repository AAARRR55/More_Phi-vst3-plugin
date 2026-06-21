#!/usr/bin/env python3
"""Check whether Ozone's Master Assistant button is actually ENABLED and the
product is licensed/authorized in this headless host. A present-but-disabled
button (or an un-authorized trial) would explain why every activation method
is a no-op. Also dump IsEnabled/IsOffscreen/HasKeyboardFocus/HelpText and the
Value/AutomationId of the button and its siblings.
"""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from pathlib import Path
import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")


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


def dump_props(c, label):
    print(f"\n--- {label} ---")
    for attr in ("ControlType", "Name", "AutomationId", "ClassName",
                 "IsEnabled", "IsOffscreen", "IsKeyboardFocusable",
                 "HasKeyboardFocus", "HelpText", "AriaRole", "AriaProperties",
                 "IsRequiredForForm", "ItemStatus"):
        try:
            v = getattr(c, attr)
            print(f"  {attr} = {v!r}")
        except Exception as e:
            print(f"  {attr} = <err {e}>")
    # full desc
    try:
        print(f"  ProcessId = {c.ProcessId}")
    except Exception:
        pass


def walk_named(c, d=0, acc=None, maxd=18):
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
            acc.append((d, x))
        except Exception:
            continue
        walk_named(x, d + 1, acc, maxd)
    return acc


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "80"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = wait_hwnd(host)
    if hwnd is None:
        print("no HWND")
        try: host.kill()
        except Exception: pass
        return 1
    print(f"HWND=0x{hwnd:x}")
    u = ctypes.windll.user32
    u.MoveWindow(hwnd, 40, 40, 1160, 600, True)
    time.sleep(3.0)

    root = ua.ControlFromHandle(hwnd)
    items = walk_named(root)
    ma = [c for (d, c) in items if (c.Name or "").strip().lower() == "master assistant" and c.ControlType == 50000]
    if not ma:
        print("no MA button")
        try: host.kill()
        except Exception: pass
        return 1

    dump_props(ma[0], "Master Assistant button")
    # Also dump a couple of known-working buttons for comparison (Options, Undo)
    for nm in ("Options", "Undo", "Preset"):
        b = [c for (d, c) in items if (c.Name or "").strip().lower() == nm.lower() and c.ControlType == 50000]
        if b:
            dump_props(b[0], f"{nm} button (comparison)")

    try: host.kill()
    except Exception: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
