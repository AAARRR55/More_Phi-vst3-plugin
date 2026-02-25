"""
Apply research-based melodic techno mixing EQ chain to Pro-Q 4 via MorphSnap MCP.
Sources: noisr.com, The Velvet Shadow, Teknup (2025), Q-Audio
"""
import socket, json, time, math, sys

TOKEN = "f6 45 b8 23 ae 20 db ef 66 68 46 c4 12 41 11 9d"
PORT = 30001
REQ_ID = 0

def next_id():
    global REQ_ID
    REQ_ID += 1
    return REQ_ID

def mcp_call(s, method, params, timeout=5):
    rid = next_id()
    req = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": rid}) + "\n"
    s.sendall(req.encode())
    time.sleep(0.15)
    data = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break
        except socket.timeout:
            break
    try:
        return json.loads(data.decode().strip())
    except:
        return {"error": "parse_failed", "raw": data.decode()[:200]}

# === Pro-Q 4 normalized value converters ===
def freq_norm(hz):
    """10 Hz - 30 kHz log scale"""
    return math.log10(hz / 10.0) / math.log10(3000.0)

def gain_norm(db):
    """-30 to +30 dB linear"""
    return (db + 30.0) / 60.0

def q_norm(q):
    """0.025 - 40 log scale"""
    return math.log10(q / 0.025) / math.log10(1600.0)

# Pro-Q 4 shape values (9 shapes, step = 1/8)
SHAPE_BELL       = 0.0
SHAPE_LOW_SHELF  = 0.125
SHAPE_LOW_CUT    = 0.25    # High-pass
SHAPE_HIGH_SHELF = 0.375
SHAPE_HIGH_CUT   = 0.5     # Low-pass
SHAPE_NOTCH      = 0.625
SHAPE_BANDPASS   = 0.75
SHAPE_TILT_SHELF = 0.875

# Pro-Q 4 slope values (6 slopes, step = 0.2)
SLOPE_6  = 0.0
SLOPE_12 = 0.2
SLOPE_18 = 0.4
SLOPE_24 = 0.6
SLOPE_36 = 0.8
SLOPE_48 = 1.0

# ============================================================
# RESEARCH-BASED MELODIC TECHNO EQ CHAIN
# Sources: noisr.com, The Velvet Shadow, Teknup, Q-Audio
# ============================================================

BANDS = [
    # (band#, freq_hz, gain_db, q, shape, slope, description)

    # --- SUBTRACTIVE (cuts first, per pro standard) ---

    (1,    25,    0.0,  1.0,  SHAPE_LOW_CUT,    SLOPE_24,
     "HP filter: remove subsonics below 25 Hz (24 dB/oct)"),

    (2,   300,   -3.0,  1.5,  SHAPE_BELL,       SLOPE_12,
     "Cut mud zone (250-500 Hz 'danger zone') — narrow Q surgical cut"),

    (3,   500,   -2.0,  1.5,  SHAPE_BELL,       SLOPE_12,
     "Cut boxiness — clean up low-mid clutter for bass clarity"),

    (4,  1000,   -1.5,  2.0,  SHAPE_BELL,       SLOPE_12,
     "Cut nasal/clashing mids — midrange clarity for synths"),

    (5,  5000,   -2.0,  1.2,  SHAPE_BELL,       SLOPE_12,
     "Tame harshness — smooth aggressive high-mids (5-8 kHz zone)"),

    # --- ADDITIVE (boosts after cuts) ---

    (6,  3500,   +1.5,  1.0,  SHAPE_BELL,       SLOPE_12,
     "Presence boost — lead cutting power, vocal clarity (3-5 kHz)"),

    (7, 12000,   +2.0,  0.71, SHAPE_HIGH_SHELF, SLOPE_12,
     "Air shelf — sparkle and openness above 12 kHz"),
]

# Output level: -2 dB gain staging for headroom (peaks at -6 to -3 dBFS)
OUTPUT_GAIN_DB = -2.0


def main():
    print("=" * 60)
    print("  MELODIC TECHNO EQ CHAIN — Research-Based")
    print("  Applying to Pro-Q 4 via MorphSnap MCP (port %d)" % PORT)
    print("=" * 60)
    print()

    # Connect
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(("127.0.0.1", PORT))
    except ConnectionRefusedError:
        print("ERROR: Cannot connect to MorphSnap on port %d" % PORT)
        sys.exit(1)

    # Authenticate
    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED:", auth)
        sys.exit(1)
    print("[OK] Authenticated to MorphSnap MCP")
    print()

    # Apply each band one at a time with verification
    print("--- Applying EQ Bands ---")
    success_count = 0

    for band_num, freq_hz, gain_db, q, shape, slope, desc in BANDS:
        base = (band_num - 1) * 24

        params_for_band = [
            (base + 0,  1.0,               "Used"),
            (base + 1,  1.0,               "Enabled"),
            (base + 2,  freq_norm(freq_hz), "Frequency"),
            (base + 3,  gain_norm(gain_db), "Gain"),
            (base + 4,  q_norm(q),          "Q"),
            (base + 5,  shape,              "Shape"),
            (base + 6,  slope,              "Slope"),
        ]

        band_ok = True
        for idx, val, name in params_for_band:
            resp = mcp_call(s, "set_parameter", {"id": idx, "value": val})
            if "error" in resp:
                print("  [FAIL] Band %d %s (idx %d): %s" % (band_num, name, idx, resp))
                band_ok = False

        # Verify Used flag
        verify = mcp_call(s, "get_parameter", {"id": base + 0})
        res = verify.get("result", {})
        used_val = res.get("value", 0) if isinstance(res, dict) else 0

        if used_val == 1.0 and band_ok:
            print("  [OK] Band %d: %s" % (band_num, desc))
            success_count += 1
        else:
            print("  [??] Band %d: applied but Used=%.1f" % (band_num, used_val))

    # Apply output level
    print()
    print("--- Gain Staging ---")
    resp = mcp_call(s, "set_parameter", {"id": 580, "value": gain_norm(OUTPUT_GAIN_DB)})
    if "error" not in resp:
        print("  [OK] Output Level: %+.1f dB (norm=%.4f)" % (OUTPUT_GAIN_DB, gain_norm(OUTPUT_GAIN_DB)))
    else:
        print("  [FAIL] Output Level:", resp)

    # Final verification — read all bands back
    print()
    print("=" * 60)
    print("  VERIFICATION — Reading back from Pro-Q 4")
    print("=" * 60)
    for band_num, freq_hz, gain_db, q, shape, slope, desc in BANDS:
        base = (band_num - 1) * 24
        vals = {}
        for offset, name in [(0, "Used"), (2, "Freq"), (3, "Gain"), (5, "Shape")]:
            r = mcp_call(s, "get_parameter", {"id": base + offset})
            res = r.get("result", {})
            vals[name] = res.get("value", -999) if isinstance(res, dict) else -999

        status = "ACTIVE" if vals["Used"] == 1.0 else "OFF"
        actual_gain_db = vals["Gain"] * 60.0 - 30.0
        print("  Band %d [%s]: Freq=%.4f  Gain=%.4f (%+.1f dB)  Shape=%.3f" % (
            band_num, status, vals["Freq"], vals["Gain"], actual_gain_db, vals["Shape"]))

    # Output level
    r = mcp_call(s, "get_parameter", {"id": 580})
    res = r.get("result", {})
    out_val = res.get("value", 0) if isinstance(res, dict) else 0
    out_db = out_val * 60.0 - 30.0
    print("  Output Level: %.4f (%+.1f dB)" % (out_val, out_db))

    s.close()

    print()
    print("=" * 60)
    print("  SUMMARY: %d/%d bands applied" % (success_count, len(BANDS)))
    print("=" * 60)
    print()
    print("  Band 1: HP 25 Hz (24 dB/oct) — subsonics removal")
    print("  Band 2: -3.0 dB @ 300 Hz Q1.5 — mud cut")
    print("  Band 3: -2.0 dB @ 500 Hz Q1.5 — boxiness cut")
    print("  Band 4: -1.5 dB @ 1 kHz Q2.0 — nasal cut")
    print("  Band 5: -2.0 dB @ 5 kHz Q1.2 — harshness tame")
    print("  Band 6: +1.5 dB @ 3.5 kHz Q1.0 — presence boost")
    print("  Band 7: +2.0 dB @ 12 kHz shelf — air")
    print("  Output: -2 dB gain staging")


if __name__ == "__main__":
    main()
