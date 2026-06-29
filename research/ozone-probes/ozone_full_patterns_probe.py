#!/usr/bin/env python3
"""Exhaustive accessibility probe of the live Ozone editor:
  - dump EVERY control (named or not), with type/rect/size
  - for each, list which UIA patterns it supports (Invoke, Toggle, SelectionItem,
    LegacyIAccessible, Value, ExpandCollapse, ScrollItem, ...)
  - flag any control with an Invoke/Toggle pattern that could be an action target
The Master Assistant button apparently has no effect via its name-click; we want
to find the ACTUAL interaction surface (a differently-named control, or a control
reachable only via a non-Invoke pattern).
"""
from __future__ import annotations
import os, re, subprocess, time
from pathlib import Path

import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")

PAT = {
    "Invoke": ua.PatternId.InvokePattern,
    "Toggle": ua.PatternId.TogglePattern,
    "Selection": ua.PatternId.SelectionItemPattern,
    "Expand": ua.PatternId.ExpandCollapsePattern,
    "LegacyIAcc": ua.PatternId.LegacyIAccessiblePattern,
    "Value": ua.PatternId.ValuePattern,
    "ScrollItem": getattr(ua.PatternId, "ScrollItemPattern", None),
}


def patterns_of(c):
    out = []
    for name, pid in PAT.items():
        if pid is None:
            continue
        try:
            if c.GetPattern(pid) is not None:
                out.append(name)
        except Exception:
            pass
    return out


def walk(c, d=0, acc=None, maxd=18):
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
        except Exception:
            continue
        pats = patterns_of(x)
        acc.append((d, ct, nm, (r.left, r.top, r.right, r.bottom), pats))
        walk(x, d + 1, acc, maxd)
    return acc


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
    env["OZONE_HOST_SECONDS"] = "90"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = wait_hwnd(host)
    if hwnd is None:
        print("no HWND")
        try:
            host.kill()
        except Exception:
            pass
        return 1
    print(f"HWND=0x{hwnd:x}")
    import ctypes
    u = ctypes.windll.user32
    u.MoveWindow(hwnd, 40, 40, 1160, 600, True)
    time.sleep(2.0)

    root = ua.ControlFromHandle(hwnd)
    items = walk(root)
    print(f"\n=== {len(items)} controls ===")

    actionable = [it for it in items if it[4]]
    print(f"\n=== {len(actionable)} ACTIONABLE controls (have a pattern) ===")
    for d, ct, nm, rect, pats in actionable:
        print(f"  {'  '*d}{ct} name={nm!r} rect={rect} pats={pats}")

    # Specifically: any control with a name suggesting the assistant / analysis
    hints = ("assistant", "play", "learn", "listen", "start", "analyze", "begin",
             "vibe", "target", "genre", "reference", "open")
    print(f"\n=== hint-matching controls (named) ===")
    for d, ct, nm, rect, pats in items:
        low = (nm or "").lower()
        if any(h in low for h in hints):
            print(f"  {'  '*d}{ct} name={nm!r} rect={rect} pats={pats}")

    try: host.kill()
    except Exception: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
