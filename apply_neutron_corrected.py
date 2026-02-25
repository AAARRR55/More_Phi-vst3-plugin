"""
CORRECTED Neutron 5 dynamics chain.
Reverse-engineered normalized mappings from screenshot calibration:
  - 0.130 ratio norm → 4.0:1 display → ratio = 1 + norm*22
  - 0.76 attack norm → 380ms display → ms = 0.419 * exp(8.96 * norm)
  - 0.75 release norm → 3750ms display → ms = 1.807 * exp(10.18 * norm)
  - 0.60 threshold norm → -32 dB display → dB = (norm - 1) * 80
"""
import socket, json, time, math, sys

TOKEN = "59 80 65 ad 07 c5 0a 93 b2 1b d7 a4 fc 2f 1d d5"
PORT = 30002
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
            if not chunk: break
            data += chunk
            if b"\n" in data: break
        except socket.timeout:
            break
    try:
        return json.loads(data.decode().strip())
    except:
        return {"error": "parse_failed"}

def set_param(s, idx, val, name=""):
    resp = mcp_call(s, "set_parameter", {"id": idx, "value": val})
    ok = "error" not in resp
    print("  [%s] [%3d] %-40s = %.4f" % ("OK" if ok else "!!", idx, name, val))
    return ok

# =====================================================
# CALIBRATED CONVERTERS (from screenshot data points)
# =====================================================

def attack_ms_to_norm(ms):
    """Calibrated: 0.0198 -> 0.5ms, 0.76 -> 380ms (exponential)"""
    return math.log(ms / 0.419) / 8.96

def release_ms_to_norm(ms):
    """Calibrated: 0.00998 -> 2ms, 0.75 -> 3750ms (exponential)"""
    return math.log(ms / 1.807) / 10.18

def ratio_to_norm(ratio):
    """Calibrated: 0.130 -> 4:1 (linear: ratio = 1 + norm*22)"""
    return (ratio - 1.0) / 22.0

def threshold_db_to_norm(db):
    """Calibrated: 0.60 -> -32dB, 1.0 -> 0dB (linear: dB = (norm-1)*80)"""
    return (db / 80.0) + 1.0


def main():
    print("=" * 60)
    print("  NEUTRON 5 — CORRECTED DYNAMICS CHAIN")
    print("  Calibrated from screenshot data points")
    print("=" * 60)

    # Print calibration verification
    print()
    print("  Calibration check:")
    print("    Attack  30ms  -> norm %.4f" % attack_ms_to_norm(30))
    print("    Release 120ms -> norm %.4f" % release_ms_to_norm(120))
    print("    Ratio   2.5:1 -> norm %.4f" % ratio_to_norm(2.5))
    print("    Thresh  -24dB -> norm %.4f" % threshold_db_to_norm(-24))
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED")
        sys.exit(1)
    print("[OK] Authenticated\n")

    # =========================================================
    # COMPRESSOR (C1) — Glue compression for melodic techno
    # Target: 2.5:1, 30ms attack, 120ms release, -24dB threshold
    # =========================================================
    print("--- COMPRESSOR (C1) — Glue ---")

    atk = attack_ms_to_norm(30)      # ~30ms
    rel = release_ms_to_norm(120)    # ~120ms
    rat = ratio_to_norm(2.5)         # 2.5:1
    thr = threshold_db_to_norm(-24)  # -24 dB
    knee = 0.30                      # soft knee (~15)

    for band_idx, base in enumerate([24, 32, 40]):
        bn = "C1 B%d" % (band_idx + 1)
        set_param(s, base + 0, 0.0,   bn + " Bypass (OFF)")
        set_param(s, base + 1, atk,   bn + " Attack (30ms)")
        set_param(s, base + 2, rat,   bn + " Ratio (2.5:1)")
        set_param(s, base + 3, rel,   bn + " Release (120ms)")
        set_param(s, base + 4, knee,  bn + " Knee (soft)")
        set_param(s, base + 5, thr,   bn + " Threshold (-24dB)")
        set_param(s, base + 6, 0.5,   bn + " Gain (0dB)")
        set_param(s, base + 7, 1.0,   bn + " Mix (100%)")

    set_param(s, 49, 0.0,  "C1 Auto Release (OFF)")
    set_param(s, 50, 1.0,  "C1 Auto Gain (ON)")
    set_param(s, 55, 0.0,  "C1 Vintage Mode (OFF)")
    set_param(s, 56, 0.5,  "C1 Global Gain (0dB)")
    set_param(s, 57, 1.0,  "C1 Global Mix (100%)")
    set_param(s, 71, 1.0,  "C1 Processing Mode (Modern)")

    print()

    # =========================================================
    # EXCITER — Subtle tape saturation
    # Target: 2-3 dB drive, warm tone, 70% mix
    # =========================================================
    print("--- EXCITER — Tape Warmth ---")

    for band_idx, base in enumerate([318, 323, 328]):
        bn = "EXC B%d" % (band_idx + 1)
        set_param(s, base + 0, 0.0,   bn + " Bypass (OFF)")
        set_param(s, base + 1, 0.5,   bn + " X")
        set_param(s, base + 2, 0.5,   bn + " Y")
        set_param(s, base + 3, 0.15,  bn + " Drive (subtle)")
        set_param(s, base + 4, 1.0,   bn + " Blend")

    set_param(s, 336, 0.70,  "EXC Global Mix (70%)")
    set_param(s, 341, 0.30,  "EXC Tame (smooth highs)")
    set_param(s, 342, 0.40,  "EXC Tone (warm)")

    print()

    # =========================================================
    # TRANSIENT SHAPER — Punch preservation
    # Target: slight attack boost, sustain control in low band
    # =========================================================
    print("--- TRANSIENT SHAPER ---")

    # B1 (low): boost attack for kick punch, tame sustain
    set_param(s, 349, 0.58,  "TS B1 Attack (boost kick)")
    set_param(s, 350, 0.0,   "TS B1 Bypass (OFF)")
    set_param(s, 351, 0.0,   "TS B1 Contour")
    set_param(s, 352, 0.42,  "TS B1 Sustain (tame)")

    # B2 (mid): slight attack boost, neutral sustain
    set_param(s, 353, 0.55,  "TS B2 Attack (slight)")
    set_param(s, 354, 0.0,   "TS B2 Bypass (OFF)")
    set_param(s, 355, 0.0,   "TS B2 Contour")
    set_param(s, 356, 0.48,  "TS B2 Sustain (neutral)")

    # B3 (high): gentle for hi-hat snap
    set_param(s, 357, 0.53,  "TS B3 Attack (gentle)")
    set_param(s, 358, 0.0,   "TS B3 Bypass (OFF)")
    set_param(s, 359, 0.0,   "TS B3 Contour")
    set_param(s, 360, 0.50,  "TS B3 Sustain (neutral)")

    set_param(s, 361, 0.5,   "TS Global Mode")
    set_param(s, 362, 0.80,  "TS Global Mix (80%)")

    print()

    # =========================================================
    # GLOBAL
    # =========================================================
    print("--- GLOBAL ---")
    set_param(s, 0, 0.0,    "Global Bypass (OFF)")
    set_param(s, 1, 0.857,  "Input Gain (unity)")
    set_param(s, 2, 0.857,  "Output Gain (unity)")

    print()
    print("=" * 60)
    print("  DONE — Corrected dynamics applied")
    print("=" * 60)
    print("  Compressor: 2.5:1 | 30ms atk | 120ms rel | -24dB thr")
    print("  Exciter: subtle drive | warm tone | 70%% mix")
    print("  Transient: kick punch + sustain control")
    print()

    s.close()


if __name__ == "__main__":
    main()
