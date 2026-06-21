#!/usr/bin/env python3
"""Decisive test of the real-audio-thread hypothesis: with genuine streaming,
does the Master Assistant panel open on a *landed* click?

Robust foreground-hold (retry AttachThreadInput, keep attached DURING the click,
verify FG==hwnd at click instant) -> click accessible "Master Assistant" ->
compare control count + screenshot before/after -> observe Frida pipeline.
"""
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


def shot(tag, l, t, w, h):
    p = str(SHOTS / (tag + ".png")).replace("/", "\\")
    ps = ("Add-Type -AssemblyName System.Windows.Forms,System.Drawing;"
          "$b=New-Object System.Drawing.Bitmap %d,%d;"
          "$g=[System.Drawing.Graphics]::FromImage($b);"
          "$g.CopyFromScreen(%d,%d,0,0,(New-Object System.Drawing.Size(%d,%d)));"
          "$g.Dispose();$b.Save('%s')") % (w, h, l, t, w, h, p)
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)
    return (SHOTS / (tag + ".png")).exists()


def is_ozone(hwnd, w):
    p = w
    for _ in range(10):
        if not p:
            return False
        if p == hwnd:
            return True
        p = u.GetParent(p)
    return False


def seize(hwnd):
    cur = k.GetCurrentThreadId()
    fg = u.GetForegroundWindow()
    if fg and not is_ozone(hwnd, fg):
        tfg = u.GetWindowThreadProcessId(fg, None)
        u.AttachThreadInput(cur, tfg, True)
    u.ShowWindow(hwnd, 5)  # SW_SHOW
    u.ShowWindow(hwnd, 9)  # SW_RESTORE
    # Force z-order WITHOUT foreground permission (SetForegroundWindow is denied,
    # but SetWindowPos HWND_TOPMOST is not) so WindowFromPoint returns ozone.
    u.SetWindowPos(hwnd, -1, 0, 0, 0, 0, 0x0013)   # HWND_TOPMOST, SWP_NOMOVE|NOSIZE|NOACTIVATE
    u.keybd_event(0x12, 0, 0, 0)        # ALT down — resets foreground idle-lock
    u.keybd_event(0x12, 0, 0x0002, 0)   # ALT up
    u.BringWindowToTop(hwnd)
    u.SetForegroundWindow(hwnd)
    u.SetFocus(hwnd)
    time.sleep(0.15)


u.WindowFromPoint.argtypes = [wintypes.POINT]


def postmsg_click(hwnd, sx, sy):
    pt = wintypes.POINT(sx, sy)
    child = u.WindowFromPoint(pt) or hwnd
    lp = (sy << 16) | (sx & 0xffff)
    u.PostMessageW(child, 0x0201, 0x0001, lp)  # WM_LBUTTONDOWN, MK_LBUTTON
    time.sleep(0.08)
    u.PostMessageW(child, 0x0202, 0, lp)        # WM_LBUTTONUP


def rect_of(h):
    r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r)); return r


def robust_click(hwnd, sx, sy):
    """PostMessage the click DIRECTLY to the ozone HWND with client-relative
    coords — bypasses z-order/foreground entirely (the on-screen window covering
    ozone is irrelevant when we address the HWND by handle). JUCE editors are a
    single HWND doing internal hit-testing."""
    seize(hwnd)
    # try the top-level HWND and any child HWND whose rect contains the point.
    targets = [hwnd]
    EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    kids = []
    def cb(h, lp):
        kids.append(h); return True
    u.EnumChildWindows(hwnd, EnumProc(cb), 0)
    for ch in kids:
        r = rect_of(ch)
        if r.left <= sx < r.right and r.top <= sy < r.bottom:
            targets.append(ch)
    for h in targets:
        r = rect_of(h)
        lp = ((sy - r.top) << 16) | ((sx - r.left) & 0xffff)
        u.PostMessageW(h, 0x0201, 0x0001, lp)  # WM_LBUTTONDOWN, MK_LBUTTON
        time.sleep(0.07)
        u.PostMessageW(h, 0x0202, 0, lp)        # WM_LBUTTONUP
        time.sleep(0.08)
    return True, f"posted to {len(targets)} win(s)"


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


def count_named(root):
    names = set()
    def walk(c):
        try: ch = c.GetChildren()
        except: ch = []
        for x in ch:
            try:
                if (x.Name or "").strip(): names.add(x.Name.strip())
            except: pass
            walk(x)
    walk(root); return names


def pid_windows(pid):
    """All top-level windows owned by the host process: (hwnd, title, rect)."""
    out = {}
    EnumProc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, lp):
        wp = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(wp))
        if wp.value == pid and u.IsWindowVisible(h):
            r = wintypes.RECT(); u.GetWindowRect(h, ctypes.byref(r))
            buf = ctypes.create_unicode_buffer(256); u.GetWindowTextW(h, buf, 256)
            out[h] = (buf.value, (r.left, r.top, r.right - r.left, r.bottom - r.top))
        return True
    u.EnumWindows(EnumProc(cb), 0)
    return out


def main():
    env = dict(os.environ); env["OZONE_HOST_SECONDS"] = "75"
    host = subprocess.Popen([EXE], stderr=subprocess.PIPE, stdout=subprocess.DEVNULL, env=env, text=True, bufsize=1)
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
    u.MoveWindow(hwnd, 30, 30, 1280, 720, True); time.sleep(2.0)

    sess = frida.get_local_device().attach(host.pid)
    sc = sess.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []
    sc.on("message", lambda m, d: tallies.append(m["payload"]["counts"])
          if m.get("type") == "send" and m["payload"].get("kind") == "tally" else None)
    sc.load(); time.sleep(4); before = dict(tallies[-1]) if tallies else {}

    root = ua.ControlFromHandle(hwnd)
    names_before = count_named(root)
    r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
    shot("panel_baseline", r.left, r.top, r.right - r.left, r.bottom - r.top)

    ma = find(root, "master assistant")
    if not ma:
        print("Master Assistant button not found in tree"); host.kill(); return 1
    b = ma[0].BoundingRectangle
    cx, cy = (b.left + b.right) // 2, (b.top + b.bottom) // 2
    print(f"Master Assistant @({b.left},{b.top}) center=({cx},{cy})")
    wins_before = pid_windows(host.pid)
    landed, diag = robust_click(hwnd, cx, cy)
    print(f"click on_ozone={landed} | {diag}")
    time.sleep(5.0)
    wins_after = pid_windows(host.pid)
    new_wins = {h: v for h, v in wins_after.items() if h not in wins_before}
    gone_wins = {h: v for h, v in wins_before.items() if h not in wins_after}
    print(f"host windows before={len(wins_before)} after={len(wins_after)}")
    if new_wins:
        print(">>> NEW host top-level window(s) appeared (panel may be separate):")
        for h, v in new_wins.items():
            print(f"    0x{h:x} title={v[0]!r} rect={v[1]}")
    else:
        print("no new host top-level window — panel did NOT open (even as a separate window)")
    r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
    shot("panel_after_ma", r.left, r.top, r.right - r.left, r.bottom - r.top)

    root2 = ua.ControlFromHandle(hwnd)
    names_after = count_named(root2)
    new = names_after - names_before
    print(f"named controls before={len(names_before)} after={len(names_after)} NEW={sorted(new)}")

    # If a wizard appeared, click through Next/Play and watch the pipeline.
    if new:
        for word in ("next", "play", "learn", "listen", "start", "continue"):
            c = find(ua.ControlFromHandle(hwnd), word)
            if c:
                bb = c[0].BoundingRectangle
                ok, mm = robust_click(hwnd, (bb.left + bb.right) // 2, (bb.top + bb.bottom) // 2)
                print(f"clicked {word!r} landed={ok} method={mm}")
                time.sleep(3.5)

    time.sleep(8)
    after = dict(tallies[-1]) if tallies else {}
    delta = {kk: after.get(kk, 0) - before.get(kk, 0) for kk in set(before) | set(after)} if before else {}
    print("DELTA:", delta)
    fired = delta.get("pipeline_root", 0) > 0 or delta.get("body", 0) > 0
    if fired: print(">>> PIPELINE FIRED — analysis started <<<")
    else: print("pipeline_root/body still 0 — panel did not engage analysis")
    try: sc.unload()
    except: pass
    sess.detach()
    try: host.kill()
    except: pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
