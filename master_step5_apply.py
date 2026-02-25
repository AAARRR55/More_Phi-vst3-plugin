"""
MASTERING CHAIN — Step 5: Pro-L 2 (Final Limiter)
Port 30005. Final brickwall limiter with -1 dBTP ceiling.
"""
import socket, json, time, sys

TOKEN = "62 6e 8e 54 f8 c3 54 8a ed 87 a7 92 dd f1 99 92"
PORT = 30005
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
    print("  MASTERING CHAIN — Step 5: Pro-L 2 (Limiter)")
    print("  Final Stage: -1 dBTP ceiling, Modern style")
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)

    # Pro-L 2 Parameter Mappings (from connect_prol2.py):
    # [0]  Gain (Input drive)
    # [1]  Style (0.7143 = Modern)
    # [2]  Lookahead
    # [9]  Oversampling (0.5 = 4x, 0.75 = 8x, 1.0 = 16x)
    # [10] True Peak Limiting (1.0 = ON)
    # [18] Output Level (Ceiling)
    
    # Ceiling -1 dBTP: norm = (dB + 30) / 30 = 29/30 = 0.9667
    # Style Modern: 0.7143
    # Lookahead: ~2ms = 0.1
    # True Peak: ON = 1.0
    # Oversampling: 8x = 0.75
    # Gain: +4dB = 4/30 = 0.1333

    print("--- FINAL LIMITER SETTINGS ---")
    set_p(s, 18, 0.9667, "Output Level (-1 dBTP)")
    set_p(s, 1,  0.7143, "Style (Modern)")
    set_p(s, 0,  0.1333, "Gain (+4dB drive)")
    set_p(s, 2,  0.1000, "Lookahead (2ms)")
    set_p(s, 10, 1.0000, "True Peak Limiting (ON)")
    set_p(s, 9,  0.7500, "Oversampling (8x)")

    s.close()
    print()
    print("=" * 60)
    print("  DONE — Full Mastering Chain Active!")
    print("=" * 60)

if __name__ == "__main__":
    main()
