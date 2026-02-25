"""
Connect to Ozone Imager via MorphSnap MCP (port 30003).
List params, then apply stereo imaging settings.
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
    except: return {"error": "parse_failed", "raw": data.decode()[:200]}

def set_p(s, idx, val, name=""):
    resp = mcp_call(s, "set_parameter", {"id": idx, "value": val})
    ok = "error" not in resp
    print("  [%s] [%3d] %-40s = %.4f" % ("OK" if ok else "!!", idx, name, val))
    return ok

# Connect
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", PORT))

auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
print("Auth:", "OK" if "result" in auth else "FAILED")

info = mcp_call(s, "get_plugin_info", {})
print("Plugin:", json.dumps(info, indent=2)[:300])

# List all parameters
params_resp = mcp_call(s, "list_parameters", {}, timeout=10)
result = params_resp.get("result", params_resp)
if isinstance(result, dict) and "parameters" in result:
    params = result["parameters"]
elif isinstance(result, list):
    params = result
else:
    params = []

print("\nTotal parameters:", len(params))

# Save and display
lines = ["Ozone Imager Parameters: %d total" % len(params), ""]
for p in params:
    idx = p.get("index", p.get("id", "?"))
    name = p.get("name", "?")
    val = p.get("value", "?")
    lines.append("[%3s] %-50s = %s" % (idx, name, val))

with open("d:/morphy/ozone_imager_params.txt", "w") as f:
    f.write("\n".join(lines))

for line in lines:
    print(line)

s.close()
print("\nDone!")
