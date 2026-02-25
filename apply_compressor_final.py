"""
FINAL CORRECTED Neutron 5 compressor — calibrated from 2 screenshot data points.
Linear mappings confirmed:
  Attack:  ms = 500 * norm  (0.4766→238ms, 0.76→380ms)
  Release: ms = 5000 * norm (0.4121→2061ms, 0.75→3750ms)
  Ratio:   ratio = 30.7 * norm (0.130→4.0:1, 0.068→2.1:1)
  Threshold: dB = (norm - 1) * 80 (0.60→-32dB, 0.70→-24dB) ✓
"""
import socket, json, time, sys

TOKEN = "59 80 65 ad 07 c5 0a 93 b2 1b d7 a4 fc 2f 1d d5"
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
    print("  [%s] [%3d] %-40s = %.5f" % ("OK" if ok else "!!", idx, name, val))

# LINEAR CONVERTERS (confirmed from 2 calibration points)
def atk(ms):   return ms / 500.0         # 30ms -> 0.060
def rel(ms):   return ms / 5000.0        # 120ms -> 0.024
def rat(r):    return r / 30.7           # 2.5:1 -> 0.081
def thr(db):   return (db / 80.0) + 1.0  # -24dB -> 0.700

def main():
    print("=" * 60)
    print("  NEUTRON 5 — FINAL CALIBRATED COMPRESSOR")
    print("=" * 60)

    # Show conversions
    print("  Attack  30ms  = %.5f" % atk(30))
    print("  Release 120ms = %.5f" % rel(120))
    print("  Ratio   2.5:1 = %.5f" % rat(2.5))
    print("  Thresh -24 dB = %.5f (already correct)" % thr(-24))
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))
    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)
    print("[OK] Authenticated\n")

    print("--- COMPRESSOR (C1) ---")

    a = atk(30)      # 0.060
    r = rel(120)     # 0.024
    ratio = rat(2.5) # 0.081
    t = thr(-24)     # 0.700
    knee = 0.30

    for band_idx, base in enumerate([24, 32, 40]):
        bn = "C1 B%d" % (band_idx + 1)
        set_p(s, base + 0, 0.0,   bn + " Bypass")
        set_p(s, base + 1, a,     bn + " Attack (30ms)")
        set_p(s, base + 2, ratio, bn + " Ratio (2.5:1)")
        set_p(s, base + 3, r,     bn + " Release (120ms)")
        set_p(s, base + 4, knee,  bn + " Knee (15)")
        set_p(s, base + 5, t,     bn + " Threshold (-24dB)")
        set_p(s, base + 6, 0.5,   bn + " Gain (0dB)")
        set_p(s, base + 7, 1.0,   bn + " Mix")

    set_p(s, 49, 0.0,  "Auto Release (OFF)")
    set_p(s, 50, 1.0,  "Auto Gain (ON)")
    set_p(s, 55, 0.0,  "Vintage Mode (OFF)")
    set_p(s, 56, 0.5,  "Global Gain")
    set_p(s, 57, 1.0,  "Global Mix")
    set_p(s, 71, 1.0,  "Processing Mode")

    s.close()

    print()
    print("=" * 60)
    print("  DONE — Check compressor GUI:")
    print("  Expect: ~2.5:1 | ~30ms attack | ~120ms release | -24dB")
    print("=" * 60)

if __name__ == "__main__":
    main()
