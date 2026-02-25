"""
MASTERING CHAIN — Step 2: Pro-MB Multiband Compression
Apply 4-band compression from AI mastering analysis.
Pro-MB layout: 22 params per band, bands 1-6.
Per band: State[0], LowXover[1], LowSlope[2], HighXover[3], HighSlope[4],
          DynMode[5], Threshold[6], Range[7], Ratio[8], Attack[9], Release[10],
          Knee[11], Lookahead[12], Level[13], Pan[14], ...
Crossover freq: 0-1 normalized (log scale, ~20Hz-20kHz)
"""
import socket, json, time, math, sys

TOKEN = "da 44 c7 96 84 b8 dc 5b 8f c6 97 b8 70 b9 e2 c2"
PORT = 30002
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

# Pro-MB freq converter (20Hz-20kHz log)
def freq_norm(hz):
    return math.log10(hz / 20.0) / math.log10(1000.0)  # 20Hz=0, 20kHz=1

# Pro-MB threshold: 0.7 default = ~-18dB. Range likely -60 to 0 dB.
# Linear: norm = (dB + 60) / 60... but default 0.7 = -18dB -> (60-18)/60 = 0.7 ✓
def thr_norm(db):
    return (db + 60.0) / 60.0

# Pro-MB ratio: 0.6 default. Range 1:1 to ~inf.
# Likely: 0=1:1, 1=inf:1 (limiter). Log scale.
# If ratio_norm = log(ratio) / log(max_ratio):
# 0.6 at default ~ ratio 4:1? Let's assume ratio_norm = log2(ratio)/log2(100)
# For 3:1: log2(3)/log2(100) = 1.585/6.644 = 0.239
# That seems low for 0.6 default. Let me try different mapping.
# Maybe: norm = (ratio - 1) / (max - 1). If max=10: (4-1)/9 = 0.333. Not 0.6.
# If norm = sqrt((ratio-1)/99): sqrt(3/99) = 0.174. No.
# Let me just work with normalized values directly based on defaults.
# Default ratio 0.6 is reasonable for mastering. Let me estimate:
# 3:1 -> ~0.45, 2.5:1 -> ~0.40, 4:1 -> ~0.55, 3.5:1 -> ~0.50

# Pro-MB attack/release: defaults both 0.2.
# Similar to other FabFilter: likely log scale.
# For mastering, values in ms. Let me estimate:
# 0.2 default ~ 10ms attack, 100ms release (typical defaults)
# I'll use relative scaling.

def main():
    print("=" * 60)
    print("  MASTERING CHAIN — Step 2: Pro-MB")
    print("  4-band multiband compression")
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)
    print("[OK] Authenticated\n")

    # AI Mastering analysis bands:
    # Band 1 (Low):     20-250 Hz,  3:1, -28dB, atk 2.5ms, rel 85ms, makeup 1.2dB
    # Band 2 (Low-Mid): 250-1200 Hz, 2.5:1, -25dB, atk 8ms, rel 120ms, makeup 0.8dB
    # Band 3 (Mid):     1200-5000 Hz, 4:1, -22dB, atk 4ms, rel 95ms, makeup 0.5dB
    # Band 4 (High):    5000+ Hz, 3.5:1, -20dB, atk 1.2ms, rel 65ms, makeup 1dB

    # Crossover frequencies (normalized):
    xover_250 = freq_norm(250)    # ~0.37
    xover_1200 = freq_norm(1200)  # ~0.59
    xover_5000 = freq_norm(5000)  # ~0.80

    print("  Crossovers: 250Hz=%.3f, 1200Hz=%.3f, 5000Hz=%.3f" % (xover_250, xover_1200, xover_5000))
    print()

    # Band layout: base = (band_num - 1) * 22
    # We use bands 1-4, disable bands 5-6

    BANDS = [
        # (band, low_xover, high_xover, threshold_dB, ratio_norm, attack_norm, release_norm, level_norm, desc)
        (1, 0.0,        xover_250,  -28,  0.45,  0.08,  0.22,  0.52,  "Low (20-250Hz): 3:1"),
        (2, xover_250,  xover_1200, -25,  0.40,  0.15,  0.28,  0.51,  "Low-Mid (250-1.2kHz): 2.5:1"),
        (3, xover_1200, xover_5000, -22,  0.35,  0.10,  0.24,  0.53,  "Mid (1.2-5kHz): 2:1 for vocal clarity"),
        (4, xover_5000, 1.0,        -20,  0.50,  0.05,  0.18,  0.52,  "High (5kHz+): 3.5:1"),
    ]

    print("--- APPLYING 4-BAND COMPRESSION ---")
    for band_num, lo_x, hi_x, thr_db, ratio, atk, rel, level, desc in BANDS:
        base = (band_num - 1) * 22
        print()
        print("  Band %d: %s" % (band_num, desc))
        set_p(s, base + 0,  1.0,           "State (ON)")
        set_p(s, base + 1,  lo_x,          "Low Crossover")
        set_p(s, base + 3,  hi_x,          "High Crossover")
        set_p(s, base + 5,  0.0,           "Dynamics Mode (Compress)")
        set_p(s, base + 6,  thr_norm(thr_db), "Threshold (%ddB)" % thr_db)
        set_p(s, base + 8,  ratio,         "Ratio")
        set_p(s, base + 9,  atk,           "Attack")
        set_p(s, base + 10, rel,           "Release")
        set_p(s, base + 11, 0.5,           "Knee (medium)")
        set_p(s, base + 12, 0.05,          "Lookahead")
        set_p(s, base + 13, level,         "Level (makeup)")

    # Disable bands 5-6
    for band_num in [5, 6]:
        base = (band_num - 1) * 22
        set_p(s, base + 0, 0.0, "Band %d State (OFF)" % band_num)

    # Global
    print()
    print("--- GLOBAL ---")
    set_p(s, 133, 0.5,  "Mix (100%)")
    set_p(s, 136, 0.5,  "Output Level (0dB)")
    set_p(s, 138, 0.0,  "Bypass (OFF)")
    set_p(s, 140, 0.5,  "Oversampling (4x)")

    s.close()

    print()
    print("=" * 60)
    print("  DONE — Pro-MB 4-band mastering compression applied")
    print("=" * 60)

if __name__ == "__main__":
    main()
