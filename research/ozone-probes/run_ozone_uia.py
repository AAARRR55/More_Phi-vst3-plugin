#!/usr/bin/env python3
"""Launch OzoneHeadlessHost (desktoped editor), grab its HWND, and traverse the
Windows UI Automation tree under it to find Ozone's Assistant/Play control.
Diagnostic: is the Assistant exposed to accessibility (=> scriptable, no human)?"""
from __future__ import annotations
import os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
import uiautomation as ua  # noqa: E402

WANT = ("assistant", "play", "learn", "listen", "analyze", "open", "start", "run", "master")


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "45"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = None
    deadline = time.time() + 25
    while time.time() < deadline and hwnd is None:
        line = host.stderr.readline()
        if not line:
            if host.poll() is not None:
                break
            continue
        print("host:", line.rstrip())
        m = re.search(r"OZONE_EDITOR_HWND=0x([0-9a-fA-F]+)", line)
        if m:
            hwnd = int(m.group(1), 16)
    if hwnd is None:
        print("FATAL: no editor HWND captured.")
        try:
            host.kill()
        except Exception:
            pass
        return 1
    print(f"\neditor HWND = 0x{hwnd:x}")

    root = ua.ControlFromHandle(hwnd)
    if root is None:
        print("ControlFromHandle returned None.")
        try:
            host.kill()
        except Exception:
            pass
        return 1
    print(f"root: {root.ControlType} name={root.Name!r} class={root.ClassName!r} "
          f"rect={root.BoundingRectangle}")

    controls = []
    def walk(c, depth=0, maxdepth=8):
        if depth > maxdepth:
            return
        try:
            children = c.GetChildren()
        except Exception:
            children = []
        for ch in children:
            try:
                ct = ch.ControlType
                nm = ch.Name or ""
                cl = ch.ClassName or ""
            except Exception:
                continue
            controls.append((depth, ct, nm, cl))
            walk(ch, depth + 1, maxdepth)
    walk(root)

    print(f"\n=== {len(controls)} descendant controls (depth<=8) ===")
    for depth, ct, nm, cl in controls[:400]:
        mark = "  <== " if any(w in (nm or "").lower() for w in WANT) else ""
        print(f"{'  '*depth}{ct} name={nm!r} class={cl!r}{mark}")

    hits = [(d, ct, nm, cl) for (d, ct, nm, cl) in controls if any(w in (nm or "").lower() for w in WANT)]
    print(f"\n=== candidate Assistant controls ({len(hits)}) ===")
    for d, ct, nm, cl in hits:
        print(f"{'  '*d}{ct} name={nm!r} class={cl!r}")

    try:
        host.kill()
    except Exception:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
