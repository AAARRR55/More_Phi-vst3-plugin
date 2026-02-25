"""
MASTERING CHAIN — Step 4: FabFilter Saturn 2 (Saturation)
Port 30004. Connect, list params, then apply tape saturation.
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
    except: return {"error": "parse_failed", "raw": data.decode()[:200]}

def set_p(s, idx, val, name=""):
    resp = mcp_call(s, "set_parameter", {"id": idx, "value": val})
    ok = "error" not in resp
    print("  [%s] [%3d] %-40s = %.4f" % ("OK" if ok else "!!", idx, name, val))

# Connect and list params first
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", PORT))

auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
print("Auth:", "OK" if "result" in auth else "FAILED")

params_resp = mcp_call(s, "list_parameters", {}, timeout=10)
result = params_resp.get("result", params_resp)
if isinstance(result, dict) and "parameters" in result:
    params = result["parameters"]
elif isinstance(result, list):
    params = result
else:
    params = []

print("\nTotal parameters:", len(params))

# Filter for key parameters: Drive, Mix, Style
keys = ["drive", "mix", "style"]
for p in params:
    name = p.get("name", "").lower()
    idx = p.get("id", p.get("index", "?"))
    val = p.get("value", "?")
    if any(k in name for k in keys):
        print("[%3s] %-50s = %s" % (idx, p.get("name"), val))

s.close()
print("\nDone!")
