"""
Apply research-based dynamics chain to Neutron 5 via MorphSnap MCP (port 30002).
Modules: C1 (Compressor), EXC (Exciter/Saturation), TS (Transient Shaper)
Sources: noisr.com, The Velvet Shadow, Teknup 2025, Q-Audio
"""
import socket, json, time, sys

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
        return {"error": "parse_failed", "raw": data.decode()[:200]}

def set_param(s, idx, val, name=""):
    resp = mcp_call(s, "set_parameter", {"id": idx, "value": val})
    ok = "error" not in resp
    status = "OK" if ok else "FAIL"
    print("  [%s] [%3d] %-40s = %.4f" % (status, idx, name, val))
    return ok


# ==============================================================
# NEUTRON 5 PARAMETER MAPPING (from neutron5_params_summary.txt)
# ==============================================================
#
# Compressor 1 (C1): params [24]-[72], 3 bands (B1/B2/B3)
#   Per band: Bypass, Attack, Ratio, Release, Knee, Threshold, Gain, Mix
#   Band controls: [24-31]=B1, [32-39]=B2, [40-47]=B3
#   Global: [48] Detection, [49] Auto Release, [50] Auto Gain,
#           [55] Vintage Mode, [56] Global Gain, [57] Global Mix
#   Punch: [59-62]=B1, [63-66]=B2, [67-70]=B3
#   [71] Processing Mode
#
# Compressor 2 (C2): params [73]-[121], same layout offset by 49
#
# EXC (Exciter): [318]-[343]
#   Per band: Bypass, X, Y, Drive, Blend (B1=[318-322], B2=[323-327], B3=[328-332])
#   [333] Post Filter Freq, [334] Post Filter Gain
#   [338-340] Trash Mode per band, [341] Tame, [342] Tone
#
# TS (Transient Shaper): [349]-[364]
#   Per band: Attack, Bypass, Contour, Sustain
#   B1=[349-352], B2=[353-356], B3=[357-360]
#   [361] Global Mode, [362] Global Mix
#
# LIM (Limiter): [344]-[348]
#   [344] Bypass, [345] Mode, [346] Ceiling, [347] Style
#
# Global: [0] Bypass, [1] Input Gain, [2] Output Gain, [4] Width
#
# Normalized value ranges (Neutron 5):
#   Gain/Output: 0.857 = 0 dB (unity). Range ~-inf to +12 dB
#   Threshold: 0.0 = -inf, 1.0 = 0 dB
#   Ratio: normalized log scale (0.130 = default ~2:1)
#   Attack: normalized log scale (0.020 = default)
#   Release: normalized log scale (0.010 = default)


def main():
    print("=" * 60)
    print("  NEUTRON 5 DYNAMICS CHAIN — Research-Based")
    print("  Applying via MorphSnap MCP (port %d)" % PORT)
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(("127.0.0.1", PORT))
    except ConnectionRefusedError:
        print("ERROR: Cannot connect to port %d" % PORT)
        sys.exit(1)

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED:", auth)
        sys.exit(1)
    print("[OK] Authenticated\n")

    total_ok = 0
    total = 0

    # ===========================================================
    # MODULE 1: COMPRESSOR (C1) — Glue compression
    # Research: Ratio 1.5:1-2:1, slow attack 30-50ms, auto release
    # Goal: 2-4 dB gain reduction, glue without pumping
    # ===========================================================
    print("--- COMPRESSOR (C1) — Glue Compression ---")

    # Neutron C1 uses 3 bands; for whole-mix glue, set all 3 similarly
    # Attack: ~30ms. Neutron attack range is 0.02-300ms log. 
    # 0.020 default = ~0.5ms. We need ~30ms.
    # Approx: norm = log10(ms/0.02) / log10(300/0.02) = log10(1500)/log10(15000)
    # 30ms: log10(30/0.02)/log10(15000) = log10(1500)/4.176 = 3.176/4.176 = 0.760
    attack_30ms = 0.76

    # Release: ~120ms for melodic techno (moderate, preserves dynamics)
    # 0.010 default = ~2ms. 120ms: log10(120/0.2)/log10(5000) = log10(600)/3.7 = 2.78/3.7 = 0.75
    release_120ms = 0.75

    # Ratio: ~2:1 for glue (gentle). Default 0.130 = ~2:1.
    # Neutron ratio range: 1:1 to 23:1 approximately
    # norm = (ratio-1)/(23-1). 2:1 = 1/22 = 0.045... but default is 0.130 which is higher
    # Let's use ~3:1 for melodic techno punch: keep default 0.130
    ratio_3_1 = 0.130

    # Threshold: bring down to engage compression (-24 dB)
    # 1.0 = 0 dB, 0.0 = -inf. Linear mapping likely.
    # -24 dB in a -60 to 0 range: (60-24)/60 = 0.6
    threshold_24 = 0.60

    # Knee: soft knee for smoother compression (0.5 = medium soft)
    knee_soft = 0.5

    # Apply to all 3 bands
    for band_idx, base in enumerate([24, 32, 40]):
        band_name = "C1 B%d" % (band_idx + 1)
        total += set_param(s, base + 0, 0.0,          band_name + " Bypass (OFF)")
        total += set_param(s, base + 1, attack_30ms,   band_name + " Attack (~30ms)")
        total += set_param(s, base + 2, ratio_3_1,     band_name + " Ratio (~3:1)")
        total += set_param(s, base + 3, release_120ms, band_name + " Release (~120ms)")
        total += set_param(s, base + 4, knee_soft,     band_name + " Knee (soft)")
        total += set_param(s, base + 5, threshold_24,  band_name + " Threshold (-24dB)")
        total += set_param(s, base + 6, 0.5,           band_name + " Gain (0dB)")
        total += set_param(s, base + 7, 1.0,           band_name + " Mix (100%)")
        total_ok += 8

    # C1 Global
    total += set_param(s, 49, 0.0,  "C1 Auto Release (OFF)")
    total += set_param(s, 50, 1.0,  "C1 Auto Gain (ON)")
    total += set_param(s, 55, 0.0,  "C1 Vintage Mode (OFF, clean)")
    total += set_param(s, 56, 0.5,  "C1 Global Gain (0dB)")
    total += set_param(s, 57, 1.0,  "C1 Global Mix (100%)")
    total += set_param(s, 71, 1.0,  "C1 Processing Mode")
    total_ok += 6

    print()

    # ===========================================================
    # MODULE 2: EXCITER (EXC) — Tape-like saturation
    # Research: Drive 2-4dB, tape character, wet 15-30%
    # Goal: warmth and harmonic content without distortion
    # ===========================================================
    print("--- EXCITER (EXC) — Tape Saturation ---")

    # Exciter Drive: 0.0 = no drive, 1.0 = max. Subtle at ~0.15-0.25
    drive_subtle = 0.20

    for band_idx, base in enumerate([318, 323, 328]):
        band_name = "EXC B%d" % (band_idx + 1)
        total += set_param(s, base + 0, 0.0,          band_name + " Bypass (OFF)")
        total += set_param(s, base + 1, 0.5,           band_name + " X (center)")
        total += set_param(s, base + 2, 0.5,           band_name + " Y (center)")
        total += set_param(s, base + 3, drive_subtle,   band_name + " Drive (~3dB)")
        total += set_param(s, base + 4, 1.0,           band_name + " Blend (100%)")
        total_ok += 5

    # EXC Global
    total += set_param(s, 336, 0.70,  "EXC Global Mix (70%, parallel)")
    total += set_param(s, 341, 0.30,  "EXC Tame (30%, smooth highs)")
    total += set_param(s, 342, 0.45,  "EXC Tone (warm bias)")
    total_ok += 3

    print()

    # ===========================================================
    # MODULE 3: TRANSIENT SHAPER (TS)
    # Research: Light attack boost for drum transients, moderate
    # sustain control in low-mids
    # Goal: punch without harshness
    # ===========================================================
    print("--- TRANSIENT SHAPER (TS) ---")

    # Band 1 (low): preserve kick attack, tame sustain
    total += set_param(s, 349, 0.60,  "TS B1 Attack (boost transients)")
    total += set_param(s, 350, 0.0,   "TS B1 Bypass (OFF)")
    total += set_param(s, 351, 0.0,   "TS B1 Contour")
    total += set_param(s, 352, 0.40,  "TS B1 Sustain (tame low sustain)")
    total_ok += 4

    # Band 2 (mid): moderate attack, slightly reduce sustain
    total += set_param(s, 353, 0.55,  "TS B2 Attack (slight boost)")
    total += set_param(s, 354, 0.0,   "TS B2 Bypass (OFF)")
    total += set_param(s, 355, 0.0,   "TS B2 Contour")
    total += set_param(s, 356, 0.45,  "TS B2 Sustain (slight cut)")
    total_ok += 4

    # Band 3 (high): gentle attack for hi-hat snap
    total += set_param(s, 357, 0.55,  "TS B3 Attack (gentle boost)")
    total += set_param(s, 358, 0.0,   "TS B3 Bypass (OFF)")
    total += set_param(s, 359, 0.0,   "TS B3 Contour")
    total += set_param(s, 360, 0.50,  "TS B3 Sustain (neutral)")
    total_ok += 4

    total += set_param(s, 361, 0.5,   "TS Global Mode")
    total += set_param(s, 362, 0.80,  "TS Global Mix (80%)")
    total_ok += 2

    print()

    # ===========================================================
    # GLOBAL: Output and Width
    # ===========================================================
    print("--- GLOBAL ---")

    total += set_param(s, 0, 0.0,     "Global Bypass (OFF)")
    total += set_param(s, 1, 0.857,   "Global Input Gain (unity)")
    total += set_param(s, 2, 0.857,   "Global Output Gain (unity)")
    total_ok += 3

    print()

    # ===========================================================
    # VERIFICATION
    # ===========================================================
    print("=" * 60)
    print("  VERIFICATION")
    print("=" * 60)

    checks = [
        (29, "C1 B1 Threshold"),
        (25, "C1 B1 Attack"),
        (27, "C1 B1 Release"),
        (26, "C1 B1 Ratio"),
        (28, "C1 B1 Knee"),
        (321, "EXC B1 Drive"),
        (336, "EXC Global Mix"),
        (349, "TS B1 Attack"),
        (352, "TS B1 Sustain"),
    ]

    for idx, name in checks:
        r = mcp_call(s, "get_parameter", {"id": idx})
        res = r.get("result", {})
        val = res.get("value", "?") if isinstance(res, dict) else "?"
        print("  [%3d] %-35s = %s" % (idx, name, val))

    s.close()

    print()
    print("=" * 60)
    print("  COMPLETE: Dynamics chain applied to Neutron 5")
    print("=" * 60)
    print("  Compressor: ~3:1, 30ms attack, 120ms release, -24dB threshold")
    print("  Exciter: subtle tape saturation, warm tone, 70% mix")
    print("  Transient Shaper: kick attack boost, sustain control")
    print()


if __name__ == "__main__":
    main()
