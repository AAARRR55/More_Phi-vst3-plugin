"""
Connect to new plugin slot — identify what's loaded and list params.
Token: b5 ca ba 09 c5 23 bc 23 6b 0a 8f b4 e3 dc 59 d4
Trying port 30006 (next after mastering chain 30001-30005).
"""
import socket, json, time

TOKEN = "b5 ca ba 09 c5 23 bc 23 6b 0a 8f b4 e3 dc 59 d4"
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
    except: return {"error": "parse_failed", "raw": data.decode()[:300]}

# Try ports 30006, 30001 (in case they replaced slot 1)
for port in [30006, 30001]:
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect(("127.0.0.1", port))
        auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
        if "result" in auth:
            print("Connected on port %d" % port)
            info = mcp_call(s, "get_plugin_info", {})
            print("Plugin:", json.dumps(info, indent=2)[:400])
            s.close()
            break
        s.close()
    except Exception as e:
        print("Port %d: %s" % (port, e))
