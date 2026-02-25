"""
MASTERING CHAIN — Step 3: Ozone Imager (Stereo Width)
Port 30003. Settings: Mono bass <150Hz, 115% width on mids/highs.
"""
import socket, json, time, sys

TOKEN = "54 a2 15 f1 b5 51 c1 72 23 ff 2d ef c6 f8 d4 37"
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
    print("  MASTERING CHAIN — Step 3: Stereo Imaging")
    print("  Setting: Mono bass, 115%% mid/high width")
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)
    print("[OK] Authenticated\n")

    # Ozone Imager Width (4-band):
    # 0.0 = mono, 0.5 = 100% (neutral), 1.0 = 200% wide
    # Target: 115% width -> 1.15 is 15% increase. 
    # norm = 0.5 + (0.15 / 2.0) = 0.575

    print("--- WIDTH SETTINGS ---")
    set_p(s, 11, 0.00,  "Band 1 Width (Full Mono <150Hz)")
    set_p(s, 12, 0.50,  "Band 2 Width (100% Low-Mids)")
    set_p(s, 13, 0.575, "Band 3 Width (115% Mids)")
    set_p(s, 14, 0.575, "Band 4 Width (115% Highs)")

    print()
    print("--- GLOBAL ---")
    set_p(s, 6,  0.0,   "Global Bypass (OFF)")
    set_p(s, 8,  1.0,   "Global Amount (100%)")

    s.close()
    print()
    print("=" * 60)
    print("  DONE — Mastering Stereo Imaging applied")
    print("=" * 60)

if __name__ == "__main__":
    main()
