"""
Scan Pro-Q 4 Band 1 params (indices 0-23) to find the Mid/Side control.
"""
import socket, json, time

TOKEN = "b5 ca ba 09 c5 23 bc 23 6b 0a 8f b4 e3 dc 59 d4"
PORT = 30001
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

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", PORT))
mcp_call(s, "initialize", {"bearer_token": TOKEN}, timeout=3)

print("Band 1 Parameters (indices 0-23):")
for i in range(24):
    r = mcp_call(s, "get_parameter", {"id": i})
    res = r.get("result", {})
    name = res.get("name", "?") if isinstance(res, dict) else "?"
    val = res.get("value", "?") if isinstance(res, dict) else "?"
    print("  [%2d] %-40s = %s" % (i, name, val))

s.close()
