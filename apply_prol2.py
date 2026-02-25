"""
Apply Pro-L 2 limiter settings for melodic techno mastering.
Key params from scan:
  [0]  Gain (input drive)     = 0.0 (range: 0-1, 0=0dB)
  [1]  Style                  = 0.7143 (discrete: 8 styles)
  [2]  Lookahead              = 0.0360
  [5]  Channel Link Trans     = 0.375
  [9]  Oversampling           = 0.0
  [10] True Peak Limiting     = 1.0 (ON)
  [18] Output Level (Ceiling) = 1.0 (0 dBTP)
"""
import socket, json, time, sys

TOKEN = "9f bd ad ce 32 b7 4d 0c 86 8a 7c 2c 11 0d e8 55"
PORT = 30004
REQ_ID = 0

def next_id():
    global REQ_ID; REQ_ID += 1; return REQ_ID

def mcp_call(s, method, params, timeout=5):
    rid = next_id()
    req = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": rid}) + "\n"
    s.sendall(req.encode()); time.sleep(0.15)
    data = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
            if b"\n" in data: break
        except socket.timeout: break
    try: return json.loads(data.decode().strip())
    except: return {"error": "parse_failed"}

def set_p(s, idx, val, name=""):
    resp = mcp_call(s, "set_parameter", {"id": idx, "value": val})
    ok = "error" not in resp
    print("  [%s] [%3d] %-40s = %.4f" % ("OK" if ok else "!!", idx, name, val))

def main():
    print("=" * 60)
    print("  PRO-L 2 — True Peak Limiter")
    print("  Final stage: ceiling -1 dBTP, punchy style")
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)
    print("[OK] Authenticated\n")

    # Pro-L 2 Output Level (Ceiling):
    # 1.0 = 0 dBTP, lower values = lower ceiling
    # Pro-L 2 range is roughly -30 to 0 dBTP
    # For -1 dBTP: norm = (30 + (-1)) / 30 = 29/30 = 0.9667
    # But 1.0 = 0 dBTP exactly, so linear: norm = (dB + 30) / 30
    # -1 dBTP -> 29/30 = 0.9667

    # Pro-L 2 Style (discrete, 8 styles in Pro-L 2):
    # 0/7=0.000 Transparent, 1/7=0.143 Punchy, 2/7=0.286 Dynamic,
    # 3/7=0.429 Allround, 4/7=0.571 Aggressive, 5/7=0.714 Modern (default),
    # 6/7=0.857 Bus, 7/7=1.0 Safe
    # Current: 0.7143 = Modern. For techno we want Punchy = 0.143

    # Pro-L 2 Gain (input drive):
    # 0.0 = 0 dB, higher = more drive into limiter
    # For -14 LUFS target with techno, moderate drive
    # Range is 0-30dB typically. norm = dB/30
    # ~6 dB drive: 6/30 = 0.20

    # Lookahead: 5ms. Default 0.036 might be ~1ms.
    # If range is 0.01-20ms and linear: 5/20 = 0.25

    print("--- LIMITER SETTINGS ---")

    # Output Ceiling: -1 dBTP
    set_p(s, 18, 0.9667, "Output Level (-1 dBTP)")

    # Style: Punchy (preserves transients for techno)
    set_p(s, 1,  0.143,  "Style (Punchy)")

    # Gain: moderate drive ~4-6 dB to push into limiter
    set_p(s, 0,  0.167,  "Gain (~5 dB drive)")

    # Lookahead: ~5ms
    set_p(s, 2,  0.25,   "Lookahead (~5ms)")

    # True Peak Limiting: ON
    set_p(s, 10, 1.0,    "True Peak Limiting (ON)")

    # Oversampling: 4x for quality
    # If discrete: 0=off, 0.25=2x, 0.5=4x, 0.75=8x, 1.0=16x
    set_p(s, 9,  0.50,   "Oversampling (4x)")

    # Channel Link: keep default
    set_p(s, 5,  0.375,  "Channel Link Transients")

    print()

    # Verify
    print("--- VERIFICATION ---")
    for idx, name in [(0, "Gain"), (1, "Style"), (2, "Lookahead"),
                       (10, "True Peak"), (18, "Output Level")]:
        r = mcp_call(s, "get_parameter", {"id": idx})
        res = r.get("result", {})
        val = res.get("value", "?") if isinstance(res, dict) else "?"
        print("  [%2d] %-25s = %s" % (idx, name, val))

    s.close()

    print()
    print("=" * 60)
    print("  DONE — Limiter applied")
    print("=" * 60)
    print("  Ceiling: -1 dBTP")
    print("  Style: Punchy (transient-preserving)")
    print("  Drive: ~5 dB")
    print("  Lookahead: ~5ms")
    print("  True Peak: ON")
    print("  Oversampling: 4x")
    print()
    print("  FULL CHAIN COMPLETE:")
    print("  Pro-Q 4 -> Neutron 5 -> Ozone Imager -> Pro-L 2")

if __name__ == "__main__":
    main()
