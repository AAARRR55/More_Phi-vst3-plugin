#!/usr/bin/env python3
"""Launch MorePhiMcpServer as a persistent child, init it (so Ozone loads),
print its PID to stdout, and keep it alive until killed."""
import os, sys, time, subprocess, json

env = dict(os.environ)
env["OZONE_VST3_PATH"] = r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"
env["OZONE_DISCOVERY_VERBOSE"] = "1"

p = subprocess.Popen(
    [r"G:\More_Phi-vst3-plugin\build\Release\MorePhiMcpServer.exe"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    env=env, text=True, bufsize=1)

print(f"MCP_PID={p.pid}", flush=True)

def send(obj):
    p.stdin.write(json.dumps(obj) + "\n"); p.stdin.flush()
    # read one line response
    line = p.stdout.readline()
    return line.strip()

# initialize -> triggers Ozone load/instantiate
r = send({"jsonrpc":"2.0","method":"initialize","params":{"bearer_token":"test"},"id":1})
print("INIT:", r[:120], flush=True)
# confirm loaded
r = send({"jsonrpc":"2.0","method":"tools/call","params":{"name":"ozone_get_parameters","arguments":{"query":"master","include_values":False}},"id":2})
print("PARAMS:", r[:160], flush=True)
print("READY: Ozone hosted; keeping server alive. Kill this process to end.", flush=True)

# stay up
try:
    while True:
        time.sleep(5)
except KeyboardInterrupt:
    pass
finally:
    p.kill()
