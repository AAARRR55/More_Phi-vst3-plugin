#!/usr/bin/env python3
"""Drive the Master Assistant wizard (user-confirmed flow: Master Assistant -> Next
-> ... -> Play), matching the window to the screenshot's coordinate space.
Clicks are foreground-held real clicks. Frida observes the analysis pipeline
(pipeline_root / body firing == analysis started). Screenshots each step."""
from __future__ import annotations
import ctypes, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path
import frida, uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")
SHOTS = ROOT / "tools" / "live_captures" / "shots"; SHOTS.mkdir(parents=True, exist_ok=True)
u = ctypes.windll.user32; k = ctypes.windll.kernel32
DOWN, UP = 0x0002, 0x0004
WIN_X, WIN_Y, WIN_W, WIN_H = 20, 20, 1130, 760  # match screenshot coord space


def fg():
    hwnd_ = None
    return hwnd_


def hold_fg(hwnd):
    fg = u.GetForegroundWindow()
    if fg != hwnd:
        tfg = u.GetWindowThreadProcessId(fg, None); tme = k.GetCurrentThreadId()
        u.AttachThreadInput(tme, tfg, True)
        u.ShowWindow(hwnd, 9); u.BringWindowToTop(hwnd); u.SetForegroundWindow(hwnd)
    return u.GetForegroundWindow() == hwnd


def click_screen(hwnd, sx, sy):
    hold_fg(hwnd); time.sleep(0.03)
    u.SetCursorPos(sx, sy); time.sleep(0.06)
    a = u.GetForegroundWindow()
    u.mouse_event(DOWN, 0, 0, 0, 0); time.sleep(0.06); u.mouse_event(UP, 0, 0, 0, 0)
    return a == hwnd


def click_ctrl(hwnd, control):
    r = control.BoundingRectangle
    return click_screen(hwnd, (r.left + r.right) // 2, (r.top + r.bottom) // 2)


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


def shot(tag):
    r = wintypes.RECT()
    # capture the whole screen region of the window
    h = None
    for _ in range(5):
        h = int(subprocess.run(["powershell", "-NoProfile", "-Command",
                ("Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
                 "$p='%s'; $b=New-Object System.Drawing.Bitmap %d,%d;"
                 "$g=[System.Drawing.Graphics]::FromImage($b);"
                 "$g.CopyFromScreen(%d,%d,0,0,(New-Object System.Drawing.Size(%d,%d)));"
                 "$g.Dispose();$b.Save($p)") % (str(SHOTS / tag) + '.png', WIN_W, WIN_H, WIN_X, WIN_Y, WIN_W, WIN_H)],
                capture_output=True, text=True).returncode)
        if (SHOTS / (tag + ".png")).exists():
            break
        time.sleep(0.3)
    return (SHOTS / (tag + ".png")).exists()


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "120"
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
    u.MoveWindow(hwnd, WIN_X, WIN_Y, WIN_W, WIN_H, True); time.sleep(1.5)

    sess = frida.get_local_device().attach(host.pid)
    sc = sess.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []
    sc.on("message", lambda m, d: tallies.append(m["payload"]["counts"])
          if m.get("type") == "send" and m["payload"].get("kind") == "tally" else None)
    sc.load(); time.sleep(4); before = dict(tallies[-1]) if tallies else {}
    root = ua.ControlFromHandle(hwnd)

    # Step 1: Master Assistant (accessible)
    ma = find(root, "master assistant")
    if ma:
        print("click Master Assistant:", click_ctrl(hwnd, ma[0]))
    time.sleep(3.5); shot("w_step1_after_ma")

    # Step 2: Next (custom-drawn, by coordinate from user screenshot: ~485,675)
    next_ok = click_screen(hwnd, WIN_X + 485, WIN_Y + 675)
    print(f"click Next @({WIN_X+485},{WIN_Y+675}) landed={next_ok}")
    time.sleep(3.5); shot("w_step2_after_next")

    # Step 3: look for Play/Listen/Open (may be accessible now or custom) -> try a few candidate coords
    # The "play the loudest portion" step usually has a central Play button ~ (490, 430) panel-rel
    for (px, py, tag) in [(485, 430, "play_center"), (485, 540, "play_lower"), (565, 380, "play_mid")]:
        click_screen(hwnd, WIN_X + px, WIN_Y + py); time.sleep(2.5)
    shot("w_step3_after_play")
    time.sleep(8)

    after = tallies[-1] if tallies else {}
    delta = {kk: after.get(kk, 0) - before.get(kk, 0) for kk in set(before) | set(after)} if before else {}
    print("tally-before:", before)
    print("tally-after:", after)
    print("DELTA:", delta)
    if delta.get("pipeline_root", 0) > 0 or delta.get("body", 0) > 0:
        print(">>> PIPELINE FIRED — headless trigger succeeded <<<")
    print("shots:", sorted(f.name for f in SHOTS.glob("w_*.png")))
    try: sc.unload()
    except: pass
    sess.detach()
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
