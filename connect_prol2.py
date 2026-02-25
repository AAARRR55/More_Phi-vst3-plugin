"""
Connect to Pro-L 2 via MorphSnap MCP (port 30004), list params, then apply limiter settings.
"""
import socket, json, time, sys

TOKEN = "9f bd ad ce 32 b7 4d 0c 86 8a 7c 2c 11 0d e8 55"
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

# Connect
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", PORT))

auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
print("Auth:", "OK" if "result" in auth else "FAILED")

info = mcp_call(s, "get_plugin_info", {})
print("Plugin:", json.dumps(info, indent=2)[:400])

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

lines = ["Pro-L 2 Parameters: %d total" % len(params), ""]
for p in params:
    idx = p.get("index", p.get("id", "?"))
    name = p.get("name", "?")
    val = p.get("value", "?")
    disc = p.get("discrete", False)
    lines.append("[%3s] %-50s = %-20s %s" % (idx, name, val, "(discrete)" if disc else ""))

with open("d:/morphy/prol2_params.txt", "w") as f:
    f.write("\n".join(lines))

for line in lines:
    print(line)

# ============================================================
# APPLY LIMITER SETTINGS
# ============================================================
# Need to identify param names first, then apply.
# Common Pro-L 2 params: Gain, Output Level (Ceiling), Style,
# Lookahead, Channel Link, Oversampling, True Peak
print("\n" + "=" * 60)
print("  APPLYING LIMITER SETTINGS")
print("=" * 60)

# Build a name->index map
name_map = {}
for p in params:
    idx = p.get("index", p.get("id"))
    name = p.get("name", "")
    name_map[name.lower()] = (idx, p.get("value", 0))

# Find key parameters
def find_param(keywords):
    for name, (idx, val) in name_map.items():
        if all(k in name for k in keywords):
            return idx, val, name
    return None, None, None

# Print what we found
print("\nKey parameters found:")
for keys, desc in [
    (["gain",], "Gain/Input"),
    (["output",], "Output/Ceiling"),
    (["style",], "Limiting Style"),
    (["lookahead",], "Lookahead"),
    (["link",], "Channel Link"),
    (["true", "peak",], "True Peak"),
    (["oversampling",], "Oversampling"),
]:
    idx, val, name = find_param(keys)
    if idx is not None:
        print("  [%3d] %-40s = %.4f (%s)" % (idx, name, val, desc))

s.close()
print("\nDone! Review params above, then I'll apply the limiter settings.")
