#!/usr/bin/env python3
"""Decisive UI test: robustly force the editor to REAL foreground (verified),
real-click 'Master Assistant', and diff the full control tree before/after to
detect whether the Assistant View opens at all. Frida observes the pipeline."""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from pathlib import Path
import frida, uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")
u = ctypes.windll.user32
k = ctypes.windll.kernel32
DOWN, UP = 0x0002, 0x0004


def force_fg(hwnd):
    for _ in range(8):
        fg = u.GetForegroundWindow()
        if fg == hwnd:
            return True
        tfg = u.GetWindowThreadProcessId(fg, None); tme = k.GetCurrentThreadId()
        u.AttachThreadInput(tme, tfg, True)
        u.ShowWindow(hwnd, 9); u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd)
        u.AttachThreadInput(tme, tfg, False)
        # nudge: press+release ALT (releases foreground lock)
        u.keybd_event(0x12, 0, 0, 0); u.keybd_event(0x12, 0, 2, 0)
        u.SetForegroundWindow(hwnd)
        time.sleep(0.25)
    return u.GetForegroundWindow() == hwnd


def real_click(x, y):
    u.SetCursorPos(x, y); time.sleep(0.1)
    u.mouse_event(DOWN, 0, 0, 0, 0); time.sleep(0.06); u.mouse_event(UP, 0, 0, 0, 0); time.sleep(0.05)


def fg_click(hwnd, control):
    """Hold foreground via AttachThreadInput THROUGH the click so it actually
    lands on the Ozone window (Windows otherwise reverts foreground in ~ms)."""
    r = control.BoundingRectangle
    x = (r.left + r.right) // 2; y = (r.top + r.bottom) // 2
    fg = u.GetForegroundWindow()
    attached = False
    if fg != hwnd:
        tfg = u.GetWindowThreadProcessId(fg, None); tme = k.GetCurrentThreadId()
        u.AttachThreadInput(tme, tfg, True); attached = True
        u.ShowWindow(hwnd, 9); u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd)
        time.sleep(0.05)
    u.SetCursorPos(x, y); time.sleep(0.05)
    actual = u.GetForegroundWindow()
    u.mouse_event(DOWN, 0, 0, 0, 0); time.sleep(0.05); u.mouse_event(UP, 0, 0, 0, 0)
    if attached:
        fg2 = u.GetForegroundWindow()
        tfg = u.GetWindowThreadProcessId(fg2, None); tme = k.GetCurrentThreadId()
        u.AttachThreadInput(tme, tme, False)  # detach
    return (actual == hwnd, x, y)


def full_tree(root):
    acc = []
    def walk(c, d=0, md=14):
        if d > md: return
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                r = x.BoundingRectangle
                acc.append((d, x.ControlType, x.Name or "", (r.left, r.top, r.right - r.left, r.bottom - r.top)))
            except: continue
            walk(x, d + 1, md)
    walk(root); return acc


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
    u.MoveWindow(hwnd, 40, 40, 1140, 580, True)
    print("foreground achieved:", force_fg(hwnd))

    sess = frida.get_local_device().attach(host.pid)
    sc = sess.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []
    sc.on("message", lambda m, d: tallies.append(m["payload"]["counts"])
          if m.get("type") == "send" and m["payload"].get("kind") == "tally" else None)
    sc.load(); time.sleep(4); before = dict(tallies[-1]) if tallies else {}

    root = ua.ControlFromHandle(hwnd)
    tree_before = full_tree(root)
    set_before = {(ct, nm) for (d, ct, nm, r) in tree_before}
    print(f"controls before: {len(tree_before)}")

    ma = find_btn(root, "master assistant")
    if ma:
        ok, x, y = fg_click(hwnd, ma[0])
        print(f"clicking Master Assistant at ({x},{y}) landed_on_ozone={ok}")
    time.sleep(4.0)

    tree_after = full_tree(root)
    set_after = {(ct, nm) for (d, ct, nm, r) in tree_after}
    new = set_after - set_before
    gone = set_before - set_after
    print(f"\ncontrols after: {len(tree_after)}  NEW: {len(new)}  GONE: {len(gone)}")
    for ct, nm in sorted(new):
        print(f"  + {ct} {nm!r}")
    for ct, nm in sorted(gone)[:20]:
        print(f"  - {ct} {nm!r}")

    # If view changed, look for Play/Learn + click
    play = [(d, ct, nm, r) for (d, ct, nm, r) in tree_after
            if any(h in nm.lower() for h in ("play", "learn", "listen", "start", "begin", "open", "analy", "vibe", "target"))
            and nm.strip().lower() != "master assistant"]
    for d, ct, nm, r in play:
        b = find_btn(root, nm.lower())
        if b:
            ok, x, y = fg_click(hwnd, b[0])
            print(f"clicked {nm!r} at ({x},{y}) landed={ok}"); time.sleep(4)

    time.sleep(16)
    after = tallies[-1] if tallies else {}
    delta = {kk: after.get(kk,0)-before.get(kk,0) for kk in set(before)|set(after)} if before else {}
    print("\nDELTA:", delta)
    if delta.get("pipeline_root",0)>0 or delta.get("body",0)>0:
        print(">>> PIPELINE FIRED <<<")
    try: sc.unload()
    except: pass
    sess.detach()
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
