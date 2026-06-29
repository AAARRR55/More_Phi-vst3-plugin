#!/usr/bin/env python3
"""Definitive headless Master-Assistant trigger.

Established facts (this session):
  - UIA Invoke on the "Master Assistant" toolbar button OPENS the Assistant
    wizard (41% pixel diff confirmed; vision verified the wizard renders).
  - The wizard's controls are custom-drawn and NOT all exposed to UIA, so we
    locate the "Next" / analysis-start button by scanning the tree for ANY
    newly-named control, and fall back to a screen-coordinate click derived
    from the live window geometry.

Flow:
  1. Launch host; show window onscreen (SW_SHOW, no foreground thrash).
  2. Attach Frida observe-only; capture pipeline tally baseline.
  3. Invoke "Master Assistant" -> wizard opens.
  4. Re-scan tree; try to Invoke any "Next"/"Play"/"Learn" control; if none
     is named, click the wizard's Next button at its known relative coord.
  5. After advancing, look for the analysis Play button; Invoke or click it.
  6. Watch the Frida tally delta: pipeline_root/body firing == success.
"""
from __future__ import annotations
import ctypes, json, os, re, subprocess, time
from ctypes import wintypes
from pathlib import Path

import frida
import uiautomation as ua

ROOT = Path(__file__).resolve().parent.parent
EXE = str(ROOT / "build" / "Release" / "OzoneHeadlessHost.exe")
JS = str(ROOT / "tools" / "ozone_capture_controller.js")
EVIDENCE = ROOT / "tools" / "live_captures" / "full_trigger_evidence.json"

u = ctypes.windll.user32
WM_LBUTTONDOWN, WM_LBUTTONUP, MK_LBUTTON = 0x0201, 0x0202, 0x0001
MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP = 0x0002, 0x0004

START_HINTS = ("play", "learn", "listen", "start", "begin", "analy", "next", "go")


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
            acc.append((d, x.ControlType, x.Name or "", x.BoundingRectangle))
        except Exception:
            continue
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


def invoke(c) -> bool:
    try:
        p = c.GetInvokePattern()
    except Exception:
        p = None
    if p is None:
        return False
    try:
        p.Invoke()
        return True
    except Exception:
        return False


def named_buttons(root):
    return sorted({nm for (d, ct, nm, r) in walk(root)
                   if ct == 50000 and nm.strip()})


def click_screen(x, y, real=True):
    if real:
        u.SetCursorPos(x, y)
        time.sleep(0.1)
        u.mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0)
        time.sleep(0.05)
        u.mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0)
    else:
        u.PostMessageW(hwnd_global, WM_LBUTTONDOWN, MK_LBUTTON, (y << 16) | (x & 0xffff))
        time.sleep(0.07)
        u.PostMessageW(hwnd_global, WM_LBUTTONUP, 0, (y << 16) | (x & 0xffff))
    time.sleep(0.1)


hwnd_global = None


def shot(hwnd, name):
    try:
        import win32gui, win32ui
        l, t, r, b = win32gui.GetWindowRect(hwnd)
        w, h = r - l, b - t
        hdc = win32gui.GetWindowDC(hwnd)
        mfc = win32ui.CreateDCFromHandle(hdc)
        save = mfc.CreateCompatibleDC()
        bmp = win32ui.CreateBitmap()
        bmp.CreateCompatibleBitmap(mfc, w, h)
        save.SelectObject(bmp)
        u.PrintWindow(hwnd, save.GetSafeHdc(), 2)
        out = str(ROOT / "tools" / "live_captures" / "shots" / name)
        bmp.SaveBitmapFile(save, out)
        save.DeleteDC(); mfc.DeleteDC(); win32gui.ReleaseDC(hwnd, hdc)
        win32gui.DeleteObject(bmp.GetHandle())
        return out
    except Exception as e:
        return f"shot-fail:{e}"


def main():
    global hwnd_global
    env = dict(os.environ)
    env["OZONE_HOST_SECONDS"] = "200"
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
    hwnd_global = hwnd
    print(f"HWND=0x{hwnd:x}")
    # Place onscreen, show without thrashing focus.
    u.MoveWindow(hwnd, 40, 40, 1160, 600, True)
    u.ShowWindow(hwnd, 5)
    time.sleep(3.0)

    # Frida observe-only on our own pid.
    session = frida.get_local_device().attach(host.pid)
    script = session.create_script(Path(JS).read_text(encoding="utf-8"))
    tallies = []

    def on_message(msg, data):
        if msg.get("type") == "send":
            p = msg["payload"]
            if isinstance(p, dict) and p.get("kind") == "tally":
                tallies.append(p["counts"])

    script.on("message", on_message)
    script.load()
    time.sleep(5)
    before = dict(tallies[-1]) if tallies else {}
    print("tally-before:", before)

    root = ua.ControlFromHandle(hwnd)
    btns0 = named_buttons(root)

    # 1) Open the Master Assistant wizard.
    ma = find_first(root, "master assistant")
    if ma is None:
        print("FATAL: no Master Assistant button")
        _finish(host, session, script, hwnd, before, {}, False, "no_ma_button")
        return 1
    print("opening Master Assistant wizard via Invoke...")
    if not invoke(ma):
        print("Invoke failed on MA")
    time.sleep(4.0)
    shot(hwnd, "step1_wizard_open.bmp")
    print("screenshot: step1_wizard_open.bmp")

    # 2) Find the Next button: by name first, else by known relative coord.
    nxt = None
    for nm in ("next", "next →", "next>", "go", "continue", "play", "learn", "listen"):
        nxt = find_first(root, nm)
        if nxt:
            print(f"found named control {nm!r}")
            break
    btns1 = named_buttons(root)
    new_btns = sorted(set(btns1) - set(btns0))
    print(f"after-open named buttons: {btns1}")
    print(f"new buttons vs before: {new_btns}")

    advanced = False
    if nxt:
        print(f"invoking {nxt.Name!r}")
        advanced = invoke(nxt)
        time.sleep(4.0)
    if not advanced:
        # Fall back: click the Next button by relative coordinate.
        # From vision: Next button ~ image (670,552) in a 1413x691 capture of a
        # window placed at (40,40). Map to current window geometry.
        import win32gui
        wl, wt, wr, wb = win32gui.GetWindowRect(hwnd)
        ww, wh = wr - wl, wb - wt
        # Vision-verified: the wizard "Next" button center is at image coords
        # (395,557) in a 1413x691 capture -> (0.2795*W, 0.8061*H) of the window.
        # Bounding box (361,536)-(429,577); close-X at (754,69)-(772,109).
        NEXT_XR, NEXT_YR = 0.2795, 0.8061
        nx = wl + int(NEXT_XR * ww)
        ny = wt + int(NEXT_YR * wh)
        print(f"no named Next -> coordinate click at screen ({nx},{ny}) [win {ww}x{wh} @ ({wl},{wt})]")
        # Real mouse needs the window foregrounded.
        fg = u.GetForegroundWindow()
        tfg = u.GetWindowThreadProcessId(fg, None)
        me = ctypes.windll.kernel32.GetCurrentThreadId()
        u.AttachThreadInput(me, tfg, True)
        try:
            u.SetForegroundWindow(hwnd)
        finally:
            u.AttachThreadInput(me, tfg, False)
        time.sleep(0.5)
        click_screen(nx, ny)
        advanced = True
        time.sleep(4.0)
        shot(hwnd, "step2_after_next.bmp")
        print("screenshot: step2_after_next.bmp")

    # 3) After advancing, look for the analysis Play/Learn control.
    started = False
    for attempt in range(3):
        for nm in ("play", "learn", "listen", "start", "begin", "analyze"):
            c = find_first(root, nm)
            if c:
                print(f"start-control found: {c.Name!r}; invoking")
                started = invoke(c)
                time.sleep(3.0)
                if started:
                    break
        if started:
            break
        time.sleep(2.0)

    # 4) Observe pipeline.
    print("\nwaiting for analysis pipeline (~30s)...")
    time.sleep(30)
    after = dict(tallies[-1]) if tallies else {}
    delta = {kk: after.get(kk, 0) - before.get(kk, 0) for kk in set(before) | set(after)} if before else {}
    print("tally-after:", after)
    print("DELTA:", delta)
    fired = delta.get("pipeline_root", 0) > 0 or delta.get("body", 0) > 0 or delta.get("hub", 0) > 0
    print(">>> ANALYSIS PIPELINE FIRED <<<" if fired else ">>> pipeline delta = 0; did not fire <<<")

    _finish(host, session, script, hwnd, before, delta, fired,
            "named_next" if (nxt and advanced) else ("coord_next" if advanced else "none"))


def _rect(hwnd):
    r = wintypes.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    return r.left, r.top, r.right, r.bottom


def _finish(host, session, script, hwnd, before, delta, fired, method):
    EVIDENCE.parent.mkdir(parents=True, exist_ok=True)
    EVIDENCE.write_text(json.dumps({
        "hwnd": hex(hwnd), "pid": host.pid,
        "tally_before": before, "delta": delta,
        "pipeline_fired": fired, "advance_method": method,
    }, indent=2), encoding="utf-8")
    print(f"evidence -> {EVIDENCE}")
    try:
        script.unload()
    except Exception:
        pass
    session.detach()
    try:
        host.kill()
    except Exception:
        pass
    return 0 if fired else 2


if __name__ == "__main__":
    raise SystemExit(main())
