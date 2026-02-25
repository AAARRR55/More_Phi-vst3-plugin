import socket, json, time, math

def mcp_call(s, method, params, req_id):
    req = json.dumps({'jsonrpc': '2.0', 'method': method, 'params': params, 'id': req_id}) + '\n'
    s.sendall(req.encode())
    time.sleep(0.5)
    data = b''
    while True:
        try:
            chunk = s.recv(65536)
            if not chunk: break
            data += chunk
            if b'\n' in data: break
        except socket.timeout:
            break
    return json.loads(data.decode().strip())

# === Pro-Q 4 Parameter Conversion ===
def freq_to_norm(hz):
    return math.log10(hz / 10.0) / math.log10(3000.0)

def gain_to_norm(db):
    return (db + 30.0) / 60.0

def q_to_norm(q):
    return math.log10(q / 0.025) / math.log10(1600.0)

# Shapes: Bell=0, LowShelf=0.125, LowCut(HP)=0.25, HighShelf=0.375
BELL = 0.0
LOW_CUT = 0.25
HIGH_SHELF = 0.375

# Slopes: 6=0.0, 12=0.2, 18=0.4, 24=0.6, 36=0.8, 48=1.0
SLOPE_24 = 0.6
SLOPE_12 = 0.2

# === Build parameter list ===
params_to_set = []

def add_band(band_num, freq_hz, gain_db, q_val, shape, slope=SLOPE_12):
    base = (band_num - 1) * 24
    params_to_set.extend([
        {'id': base + 0,  'value': 1.0},                    # Used = ON
        {'id': base + 1,  'value': 1.0},                    # Enabled = ON
        {'id': base + 2,  'value': freq_to_norm(freq_hz)},  # Frequency
        {'id': base + 3,  'value': gain_to_norm(gain_db)},  # Gain
        {'id': base + 4,  'value': q_to_norm(q_val)},       # Q
        {'id': base + 5,  'value': shape},                   # Shape
        {'id': base + 6,  'value': slope},                   # Slope
    ])

# Step 1: Gain Staging (-2 dB output trim)
params_to_set.append({'id': 580, 'value': gain_to_norm(-2.0)})

# Step 2: Subtractive EQ
add_band(1, 30,    0.0,  1.0, LOW_CUT, SLOPE_24)   # HP at 30 Hz, 24 dB/oct
add_band(2, 215,  -3.5,  1.8, BELL)                  # Mud removal
add_band(3, 431,  -2.8,  1.5, BELL)                  # Boxiness removal
add_band(4, 1034, -2.0,  2.0, BELL)                  # Nasal removal
add_band(5, 4005, -2.5,  0.7, BELL)                  # Harshness removal

# Step 5: Additive EQ (from mixing chain)
add_band(6, 10000, 2.5, 0.71, HIGH_SHELF)            # Air / brilliance

# === Connect and apply ===
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(('127.0.0.1', 30001))

# Authenticate
token = 'f6 45 b8 23 ae 20 db ef 66 68 46 c4 12 41 11 9d'
auth = mcp_call(s, 'initialize', {'bearer_token': token}, 1)
print('Auth:', 'OK' if 'result' in auth else 'FAILED')

# Apply parameters in batch
result = mcp_call(s, 'set_parameters_batch', {'parameters': params_to_set}, 2)
print('Batch result:', json.dumps(result, indent=2)[:800])

# Print summary
print()
print('=== APPLIED EQ SETTINGS ===')
print(f'Output Level: -2 dB (norm={gain_to_norm(-2.0):.4f})')
print()
bands_info = [
    ('Band 1', 'HP 30 Hz',    30,    0.0, 1.0,  'Low Cut',    '24 dB/oct'),
    ('Band 2', 'Mud',         215,  -3.5, 1.8,  'Bell',       ''),
    ('Band 3', 'Boxiness',    431,  -2.8, 1.5,  'Bell',       ''),
    ('Band 4', 'Nasal',       1034, -2.0, 2.0,  'Bell',       ''),
    ('Band 5', 'Harshness',   4005, -2.5, 0.7,  'Bell',       ''),
    ('Band 6', 'Air',         10000, 2.5, 0.71, 'High Shelf', ''),
]
for name, desc, freq, gain, q, shape, slope in bands_info:
    print(f'{name} [{desc:12s}]: {freq:5d} Hz | {gain:+.1f} dB | Q {q:.2f} | {shape}' + (f' | {slope}' if slope else ''))

# Verify by reading back band values
print()
print('=== VERIFICATION (reading back) ===')
for band in range(1, 7):
    base = (band - 1) * 24
    resp = mcp_call(s, 'get_parameter', {'id': base + 0}, 100 + band * 10)
    used_val = resp.get('result', {}).get('value', '?') if isinstance(resp.get('result'), dict) else '?'
    resp2 = mcp_call(s, 'get_parameter', {'id': base + 2}, 100 + band * 10 + 1)
    freq_val = resp2.get('result', {}).get('value', '?') if isinstance(resp2.get('result'), dict) else '?'
    resp3 = mcp_call(s, 'get_parameter', {'id': base + 3}, 100 + band * 10 + 2)
    gain_val = resp3.get('result', {}).get('value', '?') if isinstance(resp3.get('result'), dict) else '?'
    print(f'  Band {band}: Used={used_val}, Freq(norm)={freq_val}, Gain(norm)={gain_val}')

s.close()
print()
print('Done!')
