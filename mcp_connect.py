import socket, json, time, os, sys

# SECURITY FIX: Token is now loaded from environment variable MORPHSNAP_TOKEN
# DO NOT hardcode tokens in the source code.
TOKEN = os.environ.get("MORPHSNAP_TOKEN", "").strip()

def send_rpc(sock, method, params=None, req_id=1):
    msg = {"jsonrpc": "2.0", "method": method, "id": req_id}
    if params:
        msg["params"] = params
    sock.sendall((json.dumps(msg) + "\n").encode())
    
    data = b''
    sock.settimeout(10)
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                break
            data += chunk
            if b'\n' in data:
                break
    except socket.timeout:
        pass
    return json.loads(data.decode()) if data else None

if not TOKEN:
    print("[!] ERROR: No bearer token configured!")
    print("    Please set MORPHSNAP_TOKEN environment variable.")
    print("    Example: $env:MORPHSNAP_TOKEN=\"your_token_here\" (PowerShell)")
    sys.exit(1)

try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 30001))

    # Initialize with bearer token
    resp = send_rpc(s, "initialize", {"bearer_token": TOKEN}, 1)
    print("Auth:", json.dumps(resp, indent=2)[:200])

    # Get plugin info
    resp = send_rpc(s, "get_plugin_info", req_id=2)
    print("Plugin:", json.dumps(resp, indent=2)[:500])

    s.close()
except Exception as e:
    print(f"[ERROR] {e}")
