#!/usr/bin/env python3
"""Real-click trigger: move the Ozone editor onscreen, foreground it, and drive
it with REAL mouse input (SetCursorPos + mouse_event) — the only input Ozone's
view logic accepts. Click Master Assistant -> Assistant View -> Play. Frida
observes whether the analysis pipeline fires. No human; window is visible."""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path
import frida, uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")
u = ctypes.windll.user32
k = ctypes.windll.kernel32
MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x0002, 0x0004


def real_click(screen_x, screen_y):
    u.SetCursorPos(screen_x, screen_y)
    time.sleep(0.08)
    u.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
    time.sleep(0.05)
    u.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    time.sleep(0.05)


def foreground(hwnd):
    fg = u.GetForegroundWindow()
    tfg = u.GetWindowThreadProcessId(fg, None); tme = k.GetCurrentThreadId()
    u.AttachThreadInput(tme, tfg, True)
    u.ShowWindow(hwnd, 9); u.SetForegroundWindow(hwnd)
    u.AttachThreadInput(tme, tfg, False)


def find_btn(root, name):
    out = []
    def walk(c):
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                if (x.Name or "").strip().lower() == name: out.append(x)
            except: pass
            walk(x)
    walk(root); return out


def all_named(root):
    acc = []
    def walk(c, d=0, md=12):
        if d > md: return
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try: acc.append((d, x.ControlType, x.Name or "", x.BoundingRectangle))
            except: continue
            walk(x, d+1, md)
    walk(root); return acc


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "110"
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

    # Move onscreen + foreground
    u.MoveWindow(hwnd, 40, 40, 1140, 580, True)
    foreground(hwnd); time.sleep(1.0)

    sess = frida.get_local_device().attach(host.pid)
    sc = sess.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []
    sc.on("message", lambda m, d: tallies.append(m["payload"]["counts"])
          if m.get("type") == "send" and m["payload"].get("kind") == "tally" else None)
    sc.load(); time.sleep(4)
    before = dict(tallies[-1]) if tallies else {}
    print("tally-before:", before)

    root = ua.ControlFromHandle(hwnd)
    ma = find_btn(root, "master assistant")
    if ma:
        r = ma[0].BoundingRectangle
        print(f"Master Assistant screen ({r.left},{r.top})-({r.right},{r.bottom})")
        real_click((r.left + r.right)//2, (r.top + r.bottom)//2)
        print("real-clicked Master Assistant")
    time.sleep(4.0)

    # Re-scan for Assistant View / Play
    post = all_named(root)
    hints = ("play", "learn", "listen", "start", "begin", "vibe", "target", "genre", "open", "analy")
    cands = [(d,ct,nm,r) for (d,ct,nm,r) in post if any(h in nm.lower() for h in hints) and nm.strip().lower()!="master assistant"]
    print(f"\npost-open named buttons: {sorted(set(nm for d,ct,nm,r in post if ct==50000 and nm.strip()))}")
    print(f"hint candidates ({len(cands)}):")
    for d,ct,nm,r in cands:
        print(f"  {ct} {nm!r} ({r.left},{r.top})-({r.right},{r.bottom})")

    for d,ct,nm,r in cands:
        b = find_btn(root, nm.lower())
        if b:
            rr = b[0].BoundingRectangle
            real_click((rr.left+rr.right)//2, (rr.top+rr.bottom)//2)
            print(f"real-clicked {nm!r}"); time.sleep(3.5)

    time.sleep(18)
    after = tallies[-1] if tallies else {}
    delta = {kk: after.get(kk,0)-before.get(kk,0) for kk in set(before)|set(after)} if before else {}
    print("\ntally-after:", after)
    print("DELTA:", delta)
    if delta.get("pipeline_root",0)>0 or delta.get("body",0)>0:
        print(">>> ANALYSIS PIPELINE FIRED — headless(no-human) trigger succeeded <<<")
    try: sc.unload()
    except: pass
    sess.detach()
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
