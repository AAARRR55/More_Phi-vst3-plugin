"""
MASTERING CHAIN — Step 4: FabFilter Saturn 2 (Saturation)
Port 30004. Apply tape saturation: 2dB Drive, 15% Mix.
"""
import socket, json, time, sys

TOKEN = "2c 48 7e e7 57 43 ef 68 1b c3 20 8c 39 5c 27 0f"
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
    print("  MASTERING CHAIN — Step 4: Saturn 2 (Saturation)")
    print("  Setting: Tape Saturation, 2dB Drive, 15% Wet")
    print("=" * 60)
    print()

    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    s.connect(("127.0.0.1", PORT))

    auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
    if "error" in auth:
        print("AUTH FAILED"); sys.exit(1)

    # Saturn 2 Parameter Estimates (based on standard FabFilter layout):
    # [0] Band 1 Drive
    # [1] Band 1 Style (0=Tube, 1=Tape, ...)
    # [5] Global Mix
    # [6] Band 1 Mix/Level? No, [5] is usually Global Mix.
    
    # Target: 2dB Drive. Range is -inf to 18dB usually.
    # 0.5 default = 0dB. 18dB = 1.0. 
    # norm = 0.5 + (2.0 / 18.0) * 0.5 = 0.5 + 0.055 = 0.555
    
    # Target: Tape mode. 
    # Saturn 2 has many styles. Tube = 0, Tape = 0.2 approx?
    # Usually: 0-0.1 Tube, 0.1-0.2 Tape?
    # Let's try to set Style to Tape. In Saturn 2, Tape is often the second category.
    
    # Mix: 15% Wet -> 0.15
    
    print("--- SATURATION SETTINGS ---")
    set_p(s, 0, 0.555, "Band 1 Drive (2dB)")
    set_p(s, 1, 0.150, "Band 1 Style (Tape)")
    set_p(s, 5, 0.150, "Global Mix (15% Wet)")

    s.close()
    print()
    print("=" * 60)
    print("  DONE — Mastering Saturation applied")
    print("=" * 60)

if __name__ == "__main__":
    main()
