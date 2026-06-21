#!/usr/bin/env python3
"""Headless trigger via accessibility + PostMessage clicks.
Click Ozone's 'Master Assistant' button by posting WM_LBUTTONDOWN/UP to the
editor HWND at the control's client coords (offscreen-safe, no mouse move).
Dump the full control tree after to find the Play/Learn button, click it, and
observe via Frida whether the analysis pipeline fires.
"""
from __future__ import annotations
import json, os, re, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")

import ctypes
from ctypes import wintypes
import frida  # noqa: E402
import uiautomation as ua  # noqa: E402

user32 = ctypes.windll.user32
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
MK_LBUTTON = 0x0001

PLAY_HINTS = ("play", "learn", "listen", "start", "analyze", "begin", "open assistant")


def postmsg_click(top_hwnd, control):
    r = control.BoundingRectangle
    cx = (r.left + r.right) // 2
    cy = (r.top + r.bottom) // 2
    pt = wintypes.POINT(cx, cy)
    user32.ScreenToClient(top_hwnd, ctypes.byref(pt))
    lparam = (pt.y << 16) | (pt.x & 0xffff)
    user32.PostMessageW(top_hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lparam)
    time.sleep(0.05)
    user32.PostMessageW(top_hwnd, WM_LBUTTONUP, 0, lparam)
    return f"click@client({pt.x},{pt.y})"


def dump_names(root, maxdepth=10):
    names = []
    def walk(c, d):
        if d > maxdepth:
            return
        try:
            ch = c.GetChildren()
        except Exception:
            ch = []
        for x in ch:
            try:
                nm = x.Name or ""
                ct = x.ControlType
                rect = x.BoundingRectangle
            except Exception:
                continue
            names.append((d, ct, nm, (rect.left, rect.top, rect.right, rect.bottom)))
            walk(x, d + 1)
    walk(root, 0)
    return names


def find_named(root, names):
    return [(d, ct, nm, rect) for (d, ct, nm, rect) in dump_names(root)
            if any(n in nm.lower() for n in names)]


def main():
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "90"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.PIPE,
                            env=env, text=True, bufsize=1)
    hwnd = None
    t_end = time.time() + 25
    while time.time() < t_end and hwnd is None:
        line = host.stderr.readline()
        if not line:
            if host.poll() is not None:
                break
            continue
        m = re.search(r"OZONE_EDITOR_HWND=0x([0-9a-fA-F]+)", line)
        if m:
            hwnd = int(m.group(1), 16)
    if hwnd is None:
        print("FATAL: no HWND"); host.kill(); return 1
    print(f"editor HWND=0x{hwnd:x}")

    session = frida.get_local_device().attach(host.pid)
    script = session.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []

    def on_message(msg, data):
        if msg.get("type") == "send":
            p = msg["payload"]
            if p.get("kind") == "tally":
                tallies.append(p["counts"])
    script.on("message", on_message)
    script.load()
    time.sleep(4)
    before = dict(tallies[-1]) if tallies else {}
    print("tally-before:", before)

    root = ua.ControlFromHandle(hwnd)

    # 1. Click Master Assistant
    ma = [n for n in dump_names(root) if n[2].strip().lower() == "master assistant"]
    if not ma:
        print("no 'Master Assistant' control found");
        ma = find_named(root, ["master assistant"])
    print(f"Master Assistant controls: {[(d,ct,nm) for (d,ct,nm,r) in ma]}")
    target = ma[0] if ma else None
    if target:
        # re-fetch the live control by walking to match
        all_named = find_named(root, ["master assistant"])
        # find the Button-typed one
        btn = None
        def find_btn(c):
            nonlocal btn
            try: ch = c.GetChildren()
            except: ch = []
            for x in ch:
                try:
                    if (x.Name or "").strip().lower() == "master assistant":
                        btn = x; return
                except: pass
                find_btn(x);
                if btn: return
        find_btn(root)
        if btn:
            print("clicking Master Assistant:", postmsg_click(hwnd, btn))
        time.sleep(3.0)

    # 2. Dump full tree after open, look for Play/Learn button
    post = dump_names(root)
    print(f"\n=== {len(post)} controls after opening Assistant ===")
    play_cands = []
    for (d, ct, nm, rect) in post:
        low = (nm or "").lower()
        mark = " <== PLAY?" if any(h in low for h in PLAY_HINTS) else ""
        if mark or (ct == 50000 and low):  # buttons with a name
            print(f"{'  '*d}{ct} name={nm!r}{mark}")
        if any(h in low for h in PLAY_HINTS) and low != "master assistant":
            play_cands.append((d, ct, nm, rect))
    print(f"\nPlay/Learn candidates: {[(d,nm) for (d,ct,nm,r) in play_cands]}")

    # 3. Click each Play/Learn candidate
    for (d, ct, nm, rect) in play_cands:
        live = None
        def fb(c):
            nonlocal live
            try: ch = c.GetChildren()
            except: ch = []
            for x in ch:
                try:
                    if (x.Name or "").lower() == nm.lower():
                        live = x; return
                except: pass
                fb(x)
                if live: return
        fb(root)
        if live:
            print(f"clicking {nm!r}:", postmsg_click(hwnd, live))
            time.sleep(3.0)

    print("\nwaiting for analysis (~20s)...")
    time.sleep(20)
    after = tallies[-1] if tallies else {}
    print("tally-after:", after)
    if before:
        delta = {k: (after.get(k, 0) - before.get(k, 0)) for k in set(before) | set(after)}
        print("DELTA:", delta)
        if delta.get("pipeline_root", 0) > 0 or delta.get("body", 0) > 0:
            print(">>> ANALYSIS PIPELINE FIRED — headless trigger succeeded <<<")

    try: script.unload()
    except: pass
    session.detach()
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
