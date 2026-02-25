"""
FULL CHAIN VERIFICATION — reads key params from all 4 MorphSnap instances.
"""
import socket, json, time

INSTANCES = [
    {"name": "Pro-Q 4",        "port": 30001, "token": "f6 45 b8 23 ae 20 db ef 66 68 46 c4 12 41 11 9d",
     "checks": [
         (0,   "Band 1 Used (HP 25Hz)"),
         (2,   "Band 1 Freq"),
         (24,  "Band 2 Used (300Hz cut)"),
         (48,  "Band 3 Used (500Hz cut)"),
         (72,  "Band 4 Used (1kHz cut)"),
         (96,  "Band 5 Used (5kHz cut)"),
         (120, "Band 6 Used (3.5kHz boost)"),
         (144, "Band 7 Used (12kHz shelf)"),
         (580, "Output Level (-2dB)"),
     ]},
    {"name": "Neutron 5",      "port": 30002, "token": "59 80 65 ad 07 c5 0a 93 b2 1b d7 a4 fc 2f 1d d5",
     "checks": [
         (25,  "C1 B1 Attack"),
         (26,  "C1 B1 Ratio"),
         (27,  "C1 B1 Release"),
         (29,  "C1 B1 Threshold"),
         (50,  "C1 Auto Gain"),
         (321, "EXC B1 Drive"),
         (336, "EXC Global Mix"),
         (349, "TS B1 Attack"),
         (352, "TS B1 Sustain"),
     ]},
    {"name": "Ozone Imager",   "port": 30003, "token": "85 7f a2 0f b0 21 00 f9 51 db 80 a4 d5 28 fa 89",
     "checks": [
         (11,  "Band 1 Width (bass mono)"),
         (12,  "Band 2 Width (low-mids)"),
         (13,  "Band 3 Width (mids wide)"),
         (14,  "Band 4 Width (highs wide)"),
         (8,   "IMG Global Amount"),
     ]},
    {"name": "Pro-L 2",        "port": 30004, "token": "9f bd ad ce 32 b7 4d 0c 86 8a 7c 2c 11 0d e8 55",
     "checks": [
         (0,   "Gain (drive)"),
         (1,   "Style"),
         (2,   "Lookahead"),
         (10,  "True Peak Limiting"),
         (18,  "Output Level (ceiling)"),
         (9,   "Oversampling"),
     ]},
]

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

print("=" * 65)
print("  FULL MIXING CHAIN VERIFICATION")
print("  4 plugins across ports 30001-30004")
print("=" * 65)

all_ok = True
for inst in INSTANCES:
    print()
    print("--- %s (port %d) ---" % (inst["name"], inst["port"]))
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(("127.0.0.1", inst["port"]))
        auth = mcp_call(s, "initialize", {"bearer_token": inst["token"]})
        if "error" in auth:
            print("  [FAIL] Auth failed")
            all_ok = False
            continue
        print("  [OK] Connected & authenticated")

        for idx, name in inst["checks"]:
            r = mcp_call(s, "get_parameter", {"id": idx})
            res = r.get("result", {})
            if isinstance(res, dict):
                val = res.get("value", "?")
                pname = res.get("name", "")
                if isinstance(val, float):
                    print("  [%3d] %-35s = %.4f" % (idx, name, val))
                else:
                    print("  [%3d] %-35s = %s" % (idx, name, val))
            else:
                print("  [%3d] %-35s = ERROR" % (idx, name))
                all_ok = False

        s.close()
    except Exception as e:
        print("  [FAIL] Connection error: %s" % e)
        all_ok = False

print()
print("=" * 65)
if all_ok:
    print("  ALL 4 PLUGINS VERIFIED — CHAIN IS ACTIVE")
else:
    print("  WARNING: Some checks failed")
print("=" * 65)
print()
print("  Signal Flow:")
print("  Audio -> Pro-Q 4 (EQ) -> Neutron 5 (Comp+Exc+TS)")
print("       -> Ozone Imager (Stereo) -> Pro-L 2 (Limiter) -> Out")
