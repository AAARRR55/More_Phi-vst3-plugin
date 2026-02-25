"""
Apply stereo imaging to Ozone 12 Imager via MorphSnap MCP (port 30003).
4-band width control: mono bass, neutral low-mids, wide mids, wide highs.
Width Percent: 0.0 = full mono, 0.5 = no change (100%), 1.0 = max wide (200%)
"""
import socket, json, time, sys

TOKEN = "85 7f a2 0f b0 21 00 f9 51 db 80 a4 d5 28 fa 89"
PORT = 30003
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
    print("  OZONE 12 IMAGER — Stereo Imaging Chain")
    print("  Research: mono bass <150Hz, widen mids/highs")
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)
    print("[OK] Authenticated\n")

    # Ozone Imager Width Percent mapping:
    # 0.0 = full mono (0% width)
    # 0.5 = no change (100% width, unity)
    # 1.0 = max stereo (200% width)
    #
    # 4 bands (low to high):
    # Band 1 = sub/bass (<150 Hz)
    # Band 2 = low-mids (150-1kHz)
    # Band 3 = mids (1k-5kHz)
    # Band 4 = highs (>5kHz)

    print("--- WIDTH PER BAND ---")

    # Band 1 (Bass): MONO — critical for club systems
    # 0.0 = full mono, but let's keep a tiny bit: ~10% width = 0.05
    set_p(s, 11, 0.05,  "Band 1 Width (bass MONO ~10%)")

    # Band 2 (Low-Mids): Slightly narrow for focus
    # 90% width = 0.45
    set_p(s, 12, 0.45,  "Band 2 Width (low-mids 90%)")

    # Band 3 (Mids): Widen for atmospheric presence
    # 135% width = 0.675
    set_p(s, 13, 0.675, "Band 3 Width (mids 135%)")

    # Band 4 (Highs): Max width for air and sparkle
    # 145% width = 0.725
    set_p(s, 14, 0.725, "Band 4 Width (highs 145%)")

    print()
    print("--- GLOBAL ---")

    set_p(s, 6,  0.0,   "Global Bypass (OFF)")
    set_p(s, 7,  0.0,   "IMG Bypass (OFF)")
    set_p(s, 8,  1.0,   "IMG Global Amount (100%)")
    set_p(s, 16, 0.0,   "IMG Stereo/Main Bypass (OFF)")

    print()

    # Verify
    print("--- VERIFICATION ---")
    for idx, name in [(11, "Band 1 (bass)"), (12, "Band 2 (low-mid)"),
                       (13, "Band 3 (mid)"), (14, "Band 4 (high)")]:
        r = mcp_call(s, "get_parameter", {"id": idx})
        res = r.get("result", {})
        val = res.get("value", "?") if isinstance(res, dict) else "?"
        pct = val * 200 if isinstance(val, float) else "?"
        print("  [%2d] %-20s = %.4f (%s%%)" % (idx, name, val, int(pct) if isinstance(pct, float) else pct))

    s.close()

    print()
    print("=" * 60)
    print("  DONE — Stereo imaging applied")
    print("=" * 60)
    print("  Band 1 (bass <150Hz):  ~10%% width (near mono)")
    print("  Band 2 (low-mids):     90%% width (slightly narrow)")
    print("  Band 3 (mids 1-5kHz):  135%% width (atmospheric)")
    print("  Band 4 (highs >5kHz):  145%% width (air and sparkle)")

if __name__ == "__main__":
    main()
