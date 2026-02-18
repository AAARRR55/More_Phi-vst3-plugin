"""
MorphSnap — Apply EQ chain via MCP to test GUI response.
Connects to localhost:30001, lists params, applies dramatic EQ settings.
"""
import socket, json, time, sys

def rpc(method, params=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5.0)
    s.connect(('127.0.0.1', 30001))
    req = {'jsonrpc': '2.0', 'method': method, 'id': 1}
    if params:
        req['params'] = params
    s.sendall((json.dumps(req) + '\n').encode())
    time.sleep(0.3)
    chunks = []
    while True:
        try:
            d = s.recv(65536)
            if not d:
                break
            chunks.append(d)
            if b'\n' in d:
                break
        except:
            break
    s.close()
    raw = b''.join(chunks).decode().strip()
    try:
        return json.loads(raw)
    except:
        return {'raw': raw}


def main():
    print("=" * 60)
    print("MorphSnap — EQ Chain Test via MCP")
    print("=" * 60)

    # 1) Get plugin info
    info = rpc('get_plugin_info')
    print(f"\nPlugin: {json.dumps(info.get('result', info), indent=2)}")

    # 2) List ALL parameters and find EQ-related ones
    print("\nFetching parameters...")
    data = rpc('list_parameters')
    result = data.get('result', [])
    if isinstance(result, str):
        result = json.loads(result)

    total = len(result)
    print(f"Total parameters: {total}")

    # Search for EQ/filter related params
    eq_keywords = ['eq', 'filter', 'freq', 'gain', 'band', 'cutoff', 'resonance', 'q ', 'low', 'high', 'mid']
    eq_params = []
    for p in result:
        name = p['name'].lower()
        if any(kw in name for kw in eq_keywords):
            eq_params.append(p)

    print(f"\nEQ/Filter-related parameters found: {len(eq_params)}")
    for p in eq_params[:20]:
        print(f"  [{p['id']:3d}] {p['name']:40s} = {p['value']}")

    if len(eq_params) > 20:
        print(f"  ... and {len(eq_params) - 20} more")

    # 3) Apply dramatic EQ curve
    # Strategy: set any gain/level params to extreme values,
    # toggle any on/off params, adjust frequencies
    print("\n--- Applying EQ Chain ---")
    batch = []
    changes_made = 0

    for p in eq_params:
        name = p['name'].lower()
        pid = p['id']
        current = p['value']

        # Frequency params: sweep to different positions
        if 'freq' in name or 'cutoff' in name:
            new_val = 0.7 if current < 0.5 else 0.3
            batch.append({'id': pid, 'value': new_val})
            print(f"  [{pid}] {p['name']}: {current:.3f} -> {new_val:.3f}")
            changes_made += 1

        # Gain params: boost or cut dramatically
        elif 'gain' in name:
            new_val = 0.85 if current < 0.5 else 0.15
            batch.append({'id': pid, 'value': new_val})
            print(f"  [{pid}] {p['name']}: {current:.3f} -> {new_val:.3f}")
            changes_made += 1

        # Q/resonance: sharpen
        elif 'q ' in name or 'resonance' in name:
            batch.append({'id': pid, 'value': 0.8})
            print(f"  [{pid}] {p['name']}: {current:.3f} -> 0.800")
            changes_made += 1

        if changes_made >= 15:
            break

    # If no EQ params found, apply changes to first 10 params
    if changes_made == 0:
        print("\n  No EQ params found. Applying changes to first 10 params...")
        for p in result[:10]:
            pid = p['id']
            current = p['value']
            new_val = 1.0 - current  # Invert
            batch.append({'id': pid, 'value': new_val})
            print(f"  [{pid}] {p['name']}: {current:.3f} -> {new_val:.3f}")
            changes_made += 1

    # Send batch
    if batch:
        print(f"\nSending {len(batch)} parameter changes...")
        result = rpc('set_parameters_batch', {'params': batch})
        print(f"Result: {json.dumps(result.get('result', result))}")

    # 4) Capture this as snapshot slot 0
    print("\nCapturing as Snapshot 0...")
    print(json.dumps(rpc('capture_snapshot', {'slot': 0}).get('result', {})))

    # 5) Reset some params and capture as snapshot 1
    print("\nResetting for Snapshot 1 (flat)...")
    flat_batch = [{'id': b['id'], 'value': 0.5} for b in batch]
    rpc('set_parameters_batch', {'params': flat_batch})
    print(json.dumps(rpc('capture_snapshot', {'slot': 1}).get('result', {})))

    # 6) Now morph between them
    print("\nMorphing to Snapshot 0 position (x=0.0, y=0.0)...")
    rpc('set_morph_position', {'x': 0.0, 'y': 0.0})
    time.sleep(0.5)

    print("Morphing to Snapshot 1 position (x=1.0, y=0.0)...")
    rpc('set_morph_position', {'x': 1.0, 'y': 0.0})

    print("\n" + "=" * 60)
    print("CHECK THE PLUGIN GUI — the EQ curve should have changed!")
    print("=" * 60)


if __name__ == '__main__':
    main()
