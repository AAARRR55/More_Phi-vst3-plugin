"""
MorphSnap — Apply dramatic EQ with band activation via MCP.
Enables 3 bands in Pro-Q 4 and creates a smiley curve.
"""
import socket, json, time

def rpc(method, params=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(('127.0.0.1', 30001))
    req = {'jsonrpc': '2.0', 'method': method, 'id': 1}
    if params:
        req['params'] = params
    s.sendall((json.dumps(req) + '\n').encode())
    time.sleep(0.4)
    chunks = []
    while True:
        try:
            d = s.recv(65536)
            if not d: break
            chunks.append(d)
            if b'\n' in d: break
        except: break
    s.close()
    raw = b''.join(chunks).decode().strip()
    try: return json.loads(raw)
    except: return {'raw': raw}

print("Step 1: Enable Band 1 + set low freq boost (+6dB at ~100Hz)")
r = rpc('set_parameters_batch', {'params': [
    {'id': 0, 'value': 1.0},     # Band 1 Used = ON
    {'id': 1, 'value': 1.0},     # Band 1 Enabled = ON
    {'id': 2, 'value': 0.25},    # Band 1 Frequency = low (~100Hz)
    {'id': 3, 'value': 0.75},    # Band 1 Gain = +6dB boost
    {'id': 4, 'value': 0.4},     # Band 1 Q = moderate
    {'id': 5, 'value': 0.0},     # Band 1 Shape = Bell
]})
print(f"  Result: {r.get('result', r)}")

time.sleep(0.3)

print("Step 2: Enable Band 2 + set mid freq cut (-4dB at ~1kHz)")
r = rpc('set_parameters_batch', {'params': [
    {'id': 24, 'value': 1.0},    # Band 2 Used = ON
    {'id': 25, 'value': 1.0},    # Band 2 Enabled = ON
    {'id': 26, 'value': 0.50},   # Band 2 Frequency = mid (~1kHz)
    {'id': 27, 'value': 0.35},   # Band 2 Gain = -3dB cut
    {'id': 28, 'value': 0.5},    # Band 2 Q = moderate
    {'id': 29, 'value': 0.0},    # Band 2 Shape = Bell
]})
print(f"  Result: {r.get('result', r)}")

time.sleep(0.3)

print("Step 3: Enable Band 3 + set high freq boost (+4dB at ~8kHz)")
r = rpc('set_parameters_batch', {'params': [
    {'id': 48, 'value': 1.0},    # Band 3 Used = ON
    {'id': 49, 'value': 1.0},    # Band 3 Enabled = ON
    {'id': 50, 'value': 0.78},   # Band 3 Frequency = high (~8kHz)
    {'id': 51, 'value': 0.68},   # Band 3 Gain = +4dB boost
    {'id': 52, 'value': 0.4},    # Band 3 Q = moderate
    {'id': 53, 'value': 0.0},    # Band 3 Shape = Bell
]})
print(f"  Result: {r.get('result', r)}")

print("\nDone! Pro-Q 4 should show a SMILEY CURVE:")
print("  Band 1: +6dB boost at low freqs")
print("  Band 2: -3dB cut at mid freqs")
print("  Band 3: +4dB boost at high freqs")
