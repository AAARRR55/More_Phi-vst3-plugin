import socket, json, time

def mcp_call(s, method, params, req_id):
    req = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": req_id}) + "\n"
    s.sendall(req.encode())
    time.sleep(0.3)
    data = b""
    while True:
        try:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
            if b"\n" in data: break
        except socket.timeout:
            break
    return json.loads(data.decode().strip())

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", 30001))

token = "f6 45 b8 23 ae 20 db ef 66 68 46 c4 12 41 11 9d"
mcp_call(s, "initialize", {"bearer_token": token}, 1)

print("=== PRO-Q 4 BAND STATUS ===")
for band in range(1, 7):
    base = (band - 1) * 24
    vals = {}
    for offset, name in [(0, "Used"), (2, "Freq"), (3, "Gain"), (4, "Q"), (5, "Shape"), (6, "Slope")]:
        r = mcp_call(s, "get_parameter", {"id": base + offset}, band * 100 + offset)
        res = r.get("result", {})
        if isinstance(res, dict):
            vals[name] = res.get("value", "?")
        else:
            vals[name] = "?"
    
    used = vals["Used"]
    status = "ACTIVE" if used == 1.0 else "inactive"
    print("Band %d [%s]: Freq=%.4f  Gain=%.4f  Q=%.4f  Shape=%.3f  Slope=%.3f" % (
        band, status, vals["Freq"], vals["Gain"], vals["Q"], vals["Shape"], vals["Slope"]))

# Check output level
r = mcp_call(s, "get_parameter", {"id": 580}, 999)
res = r.get("result", {})
if isinstance(res, dict):
    print("\nOutput Level: %.4f" % res.get("value", 0))

# Check bands 7-8 to confirm they are still inactive
for band in [7, 8]:
    base = (band - 1) * 24
    r = mcp_call(s, "get_parameter", {"id": base}, band * 100)
    res = r.get("result", {})
    used = res.get("value", 0) if isinstance(res, dict) else 0
    print("Band %d: Used=%.1f (should be 0.0)" % (band, used))

s.close()
print("\nDone!")
