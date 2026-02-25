"""
MASTERING CHAIN — Step 1: Pro-Q 4 Linear Phase EQ
Port 30001. Settings from AI mastering analysis.
"""
import socket, json, time, math, sys

TOKEN = "86 ae 38 76 d7 ab 77 d5 d8 8a 1f d9 83 03 1f 20"
PORT = 30001
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

# Pro-Q 4 converters (calibrated from mixing chain)
def freq_norm(hz):
    return math.log10(hz / 10.0) / math.log10(3000.0)

def gain_norm(db):
    return (db + 30.0) / 60.0

def q_norm(q):
    return math.log10(q / 0.025) / math.log10(1600.0)

# Shapes
BELL = 0.0
LOW_CUT = 0.25
HIGH_SHELF = 0.375

# Slopes
SLOPE_12 = 0.2
SLOPE_24 = 0.6

def main():
    print("=" * 60)
    print("  MASTERING CHAIN — Step 1: Linear Phase EQ")
    print("  Pro-Q 4 on port %d" % PORT)
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)
    print("[OK] Authenticated\n")

    # Set Processing Mode to Linear Phase
    # Pro-Q 4: [576] Processing Mode
    # 0.0=Zero Latency, 0.5=Natural, 1.0=Linear Phase
    print("--- PROCESSING MODE ---")
    set_p(s, 576, 1.0, "Processing Mode (Linear Phase)")
    print()

    # AI Mastering Analysis bands (ADJUSTED FOR VOCAL PRESENCE):
    # Band 1: HP 18 Hz — remove subsonic rumble
    # Band 2: Bell 285 Hz, -2.1 dB, Q 1.4 — general mud removal
    # Band 3: Bell 1200 Hz, +1.5 dB, Q 0.8 — vocal body and presence (NEW)
    # Band 4: Bell 3200 Hz, +2.5 dB, Q 1.0 — presence and clarity (INCREASED)
    # Band 5: High-shelf 12000 Hz, +1.2 dB, Q 0.71 — brilliance

    BANDS = [
        (1, 18,    0.0,  1.0,  LOW_CUT,    SLOPE_24,
         "HP 18 Hz — subsonic removal"),
        (2, 285,  -2.1,  1.4,  BELL,       SLOPE_12,
         "-2.1 dB @ 285 Hz — mud removal"),
        (3, 1200, +1.5,  0.8,  BELL,       SLOPE_12,
         "+1.5 dB @ 1.2 kHz — vocal presence"),
        (4, 3200, +2.5,  1.0,  BELL,       SLOPE_12,
         "+2.5 dB @ 3.2 kHz — presence/clarity"),
        (5, 12000, +1.2, 0.71, HIGH_SHELF, SLOPE_12,
         "+1.2 dB @ 12 kHz shelf — brilliance"),
    ]

    print("--- EQ BANDS ---")
    for band_num, freq_hz, gain_db, q, shape, slope, desc in BANDS:
        base = (band_num - 1) * 24
        set_p(s, base + 0, 1.0,                "Band %d Used (ON)" % band_num)
        set_p(s, base + 1, 1.0,                "Band %d Enabled" % band_num)
        set_p(s, base + 2, freq_norm(freq_hz),  "Band %d Freq (%d Hz)" % (band_num, freq_hz))
        set_p(s, base + 3, gain_norm(gain_db),  "Band %d Gain (%+.1f dB)" % (band_num, gain_db))
        set_p(s, base + 4, q_norm(q),           "Band %d Q (%.2f)" % (band_num, q))
        set_p(s, base + 5, shape,               "Band %d Shape" % band_num)
        set_p(s, base + 6, slope,               "Band %d Slope" % band_num)
        print("  -> %s" % desc)
        print()

    # Ensure other bands are OFF
    for band_num in range(len(BANDS) + 1, 9):
        base = (band_num - 1) * 24
        set_p(s, base + 0, 0.0, "Band %d Used (OFF)" % band_num)

    # Verify
    print("--- VERIFICATION ---")
    for band_num in range(1, len(BANDS) + 1):
        base = (band_num - 1) * 24
        r = mcp_call(s, "get_parameter", {"id": base})
        res = r.get("result", {})
        used = res.get("value", 0) if isinstance(res, dict) else 0
        r2 = mcp_call(s, "get_parameter", {"id": base + 2})
        res2 = r2.get("result", {})
        freq = res2.get("value", 0) if isinstance(res2, dict) else 0
        print("  Band %d: Used=%.0f, Freq=%.4f" % (band_num, used, freq))

    r = mcp_call(s, "get_parameter", {"id": 576})
    res = r.get("result", {})
    mode = res.get("value", "?") if isinstance(res, dict) else "?"
    print("  Processing Mode: %s (1.0=Linear Phase)" % mode)

    s.close()

    print()
    print("=" * 60)
    print("  DONE — Mastering LP EQ applied (4 bands)")
    print("=" * 60)

if __name__ == "__main__":
    main()
