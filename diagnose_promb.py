import socket, json, time, sys

TOKEN = "da 44 c7 96 84 b8 dc 5b 8f c6 97 b8 70 b9 e2 c2"
PORT = 30002

def mcp_call(s, method, params, rid):
    req = json.dumps({"jsonrpc": "2.0", "method": method, "params": params, "id": rid}) + "\n"
    s.sendall(req.encode())
    time.sleep(0.1)
    return s.recv(65536).decode()

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(("127.0.0.1", PORT))
mcp_call(s, "initialize", {"bearer_token": TOKEN}, 1)

print("Reading Pro-MB Band States:")
for i in range(4):
    base = i * 22
    resp = json.loads(mcp_call(s, "get_parameter", {"id": base}, i+2))
    val = resp["result"]["value"]
    name = resp["result"]["name"]
    
    resp2 = json.loads(mcp_call(s, "get_parameter", {"id": base+3}, i+10))
    xover = resp2["result"]["value"]
    
    print(f"  {name}: {val} | High Xover: {xover}")

s.close()
