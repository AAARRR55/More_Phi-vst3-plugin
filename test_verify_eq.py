"""Verify Pro-Q 4 parameters were actually set."""
import socket, json, time

def rpc(method, params=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(('127.0.0.1', 30001))
    req = {'jsonrpc': '2.0', 'method': method, 'id': 1}
    if params: req['params'] = params
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
    return json.loads(b''.join(chunks).decode().strip())

# Read back the band parameters we just set
ids_to_check = [0, 1, 2, 3, 4, 5, 24, 25, 26, 27, 48, 49, 50, 51]
expected = {
    0: 1.0, 1: 1.0, 2: 0.25, 3: 0.75, 4: 0.4, 5: 0.0,
    24: 1.0, 25: 1.0, 26: 0.50, 27: 0.35,
    48: 1.0, 49: 1.0, 50: 0.78, 51: 0.68,
}

all_params = rpc('list_parameters').get('result', [])
print(f"Total params: {len(all_params)}")
print(f"\n{'ID':>4} {'Name':40s} {'Current':>8} {'Expected':>9} {'Match':>6}")
print("-" * 70)

mismatches = 0
for pid in ids_to_check:
    p = all_params[pid]
    cur = p['value']
    exp = expected[pid]
    match = abs(cur - exp) < 0.01
    status = "OK" if match else "MISMATCH"
    if not match: mismatches += 1
    print(f"{pid:4d} {p['name']:40s} {cur:8.4f} {exp:9.4f} {status:>6}")

print(f"\n{'ALL MATCHED' if mismatches == 0 else f'{mismatches} MISMATCHES'}")
