#!/usr/bin/env python3
"""Open the Master Assistant wizard via Invoke, then dump EVERY control in the
tree (named or not) that supports Invoke / LegacyIAccessible / Toggle — the
wizard's "Next" / "Play" buttons must expose at least one of these even if they
have no Name. We print their rect, type, name (if any), and supported patterns,
so we can Invoke the right one headlessly without coordinate guessing.
"""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from pathlib import Path
import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
u = ctypes.windll.user32


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


def patterns(c):
    out = []
    for name, pid in (("Invoke", ua.PatternId.InvokePattern),
                      ("Toggle", ua.PatternId.TogglePattern),
                      ("Selection", ua.PatternId.SelectionItemPattern),
                      ("LegacyIAcc", ua.PatternId.LegacyIAccessiblePattern),
                      ("Value", ua.PatternId.ValuePattern)):
        try:
            if c.GetPattern(pid) is not None:
                out.append(name)
        except Exception:
            pass
    return out


def walk(c, d=0, acc=None, maxd=20):
    if acc is None:
        acc = []
    if d > maxd:
        return acc
    try:
        kids = c.GetChildren()
    except Exception:
        kids = []
    for x in kids:
        acc.append((d, x))
        walk(x, d + 1, acc, maxd)
    return acc


def find_first(root, name_lower):
    out = []

    def rec(c):
        if out:
            return
        try:
            kids = c.GetChildren()
        except Exception:
            kids = []
        for x in kids:
            try:
                if (x.Name or "").strip().lower() == name_lower:
                    out.append(x)
            except Exception:
                pass
            rec(x)
            if out:
                return

    rec(root)
    return out[0] if out else None


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "70"
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
    u.MoveWindow(hwnd, 40, 40, 1160, 600, True)
    u.ShowWindow(hwnd, 5)
    time.sleep(3.0)

    root = ua.ControlFromHandle(hwnd)
    ma = find_first(root, "master assistant")
    print("MA:", ma is not None)
    try:
        ma.GetInvokePattern().Invoke()
        print("Invoke OK")
    except Exception as e:
        print("Invoke err", e)
    time.sleep(4.5)

    # Dump full tree, classify.
    items = walk(root)
    print(f"\n=== {len(items)} total controls in tree after wizard open ===")
    actionable = []
    for d, c in items:
        try:
            pats = patterns(c)
        except Exception:
            pats = []
        if not pats:
            continue
        try:
            nm = c.Name or ""
            ct = c.ControlType
            r = c.BoundingRectangle
            rect = (r.left, r.top, r.right, r.bottom)
            aid = c.AutomationId or ""
        except Exception:
            continue
        # Skip the always-present toolbar (we know those). Focus on new/large/unnamed.
        w_ = rect[2] - rect[0]
        h_ = rect[3] - rect[1]
        actionable.append((d, ct, nm, aid, rect, (w_, h_), pats))

    print(f"\n=== {len(actionable)} actionable controls (with patterns) ===")
    for d, ct, nm, aid, rect, wh, pats in actionable:
        flag = ""
        if wh[0] >= 30 and wh[1] >= 20:
            flag = "  <== sized-control"
        print(f"  {'  '*d}{ct} name={nm!r} aid={aid!r} rect={rect} size={wh} pats={pats}{flag}")

    try:
        host.kill()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
