"""
Apply Mid-only vocal boost bands to Pro-Q 4 (port 30001).
Adds bands 6-7 with Mid channel processing for vocal presence.
"""
import socket, json, time, math, sys

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

def set_p(s, idx, val, name=""):
    resp = mcp_call(s, "set_parameter", {"id": idx, "value": val})
    ok = "error" not in resp
    print("  [%s] [%3d] %-40s = %.4f" % ("OK" if ok else "!!", idx, name, val))

def freq_norm(hz):
    return math.log10(hz / 10.0) / math.log10(3000.0)

def gain_norm(db):
    return (db + 30.0) / 60.0

def q_norm(q):
    return math.log10(q / 0.025) / math.log10(1600.0)

BELL = 0.0
# Stereo Placement: 0=Stereo, ~0.75=Mid (Pro-Q 4 typically uses discrete steps)
MID_ONLY = 0.75

s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(("127.0.0.1", PORT))
mcp_call(s, "initialize", {"bearer_token": TOKEN})

print("=" * 60)
print("  MID-ONLY VOCAL BOOST (Bands 6-7)")
print("=" * 60)

# Band 6: Mid-only boost at 2 kHz (+2.5 dB) — vocal presence
base = 5 * 24  # Band 6
set_p(s, base + 0, 1.0,              "Band 6 Used (ON)")
set_p(s, base + 1, 1.0,              "Band 6 Enabled")
set_p(s, base + 2, freq_norm(2000),  "Band 6 Freq (2000 Hz)")
set_p(s, base + 3, gain_norm(2.5),   "Band 6 Gain (+2.5 dB)")
set_p(s, base + 4, q_norm(1.2),      "Band 6 Q (1.2)")
set_p(s, base + 5, BELL,             "Band 6 Shape (Bell)")
set_p(s, base + 7, MID_ONLY,         "Band 6 Stereo (MID ONLY)")
print("  -> +2.5 dB @ 2 kHz MID ONLY — vocal presence")
print()

# Band 7: Mid-only boost at 4 kHz (+2 dB) — vocal clarity
base = 6 * 24  # Band 7
set_p(s, base + 0, 1.0,              "Band 7 Used (ON)")
set_p(s, base + 1, 1.0,              "Band 7 Enabled")
set_p(s, base + 2, freq_norm(4000),  "Band 7 Freq (4000 Hz)")
set_p(s, base + 3, gain_norm(2.0),   "Band 7 Gain (+2.0 dB)")
set_p(s, base + 4, q_norm(1.0),      "Band 7 Q (1.0)")
set_p(s, base + 5, BELL,             "Band 7 Shape (Bell)")
set_p(s, base + 7, MID_ONLY,         "Band 7 Stereo (MID ONLY)")
print("  -> +2.0 dB @ 4 kHz MID ONLY — vocal clarity")

s.close()
print()
print("=" * 60)
print("  DONE — Mid-only vocal boost applied")
print("=" * 60)
