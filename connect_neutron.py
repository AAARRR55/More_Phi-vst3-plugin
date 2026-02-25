"""
Connect to MorphSnap instance 2 (Neutron 5) on port 30002.
List all parameters, save to file, then apply dynamics chain.
"""
import socket, json, time, math, sys

TOKEN = "59 80 65 ad 07 c5 0a 93 b2 1b d7 a4 fc 2f 1d d5"
PORT = 30002
REQ_ID = 0

def next_id():
    global REQ_ID
    REQ_ID += 1
    return REQ_ID

def mcp_call(s, method, params, timeout=5):
    rid = next_id()
    req = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": rid}) + "\n"
    s.sendall(req.encode())
    time.sleep(0.2)
    data = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
            if b"\n" in data:
                break
        except socket.timeout:
            break
    try:
        return json.loads(data.decode().strip())
    except:
        return {"error": "parse_failed", "raw": data.decode()[:200]}

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", PORT))

# Authenticate
auth = mcp_call(s, "initialize", {"bearer_token": TOKEN})
print("Auth:", json.dumps(auth, indent=2)[:500])

# Get plugin info
info = mcp_call(s, "get_plugin_info", {})
print("\nPlugin Info:", json.dumps(info, indent=2)[:500])

# List parameters
params_resp = mcp_call(s, "list_parameters", {}, timeout=10)

result = params_resp.get("result", params_resp)
if isinstance(result, dict) and "parameters" in result:
    params = result["parameters"]
elif isinstance(result, list):
    params = result
else:
    params = []

print("\nTotal parameters:", len(params))

# Save to file
with open("d:/morphy/neutron5_params.json", "w") as f:
    json.dump(params_resp, f, indent=2)

# Print all parameter names grouped
lines = []
lines.append("Neutron 5 Parameters: %d total" % len(params))
lines.append("")
for p in params:
    idx = p.get("index", p.get("id", "?"))
    name = p.get("name", "?")
    val = p.get("value", "?")
    lines.append("[%3s] %-50s = %s" % (idx, name, val))

with open("d:/morphy/neutron5_params_summary.txt", "w") as f:
    f.write("\n".join(lines))

# Print first 80 to console
for line in lines[:80]:
    print(line)
if len(lines) > 80:
    print("... and %d more (see neutron5_params_summary.txt)" % (len(lines) - 80))

s.close()
print("\nDone!")
