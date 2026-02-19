"""
MorphSnap Multi-Instance Test
Verifies that two plugin instances running concurrently have:
  - Distinct ports
  - Distinct instance IDs and morph codes
  - Isolated bearer token authentication (cross-auth must fail)

Usage:
  1. Load TWO instances of MorphSnap in FL Studio
  2. Run: python test_multi_instance.py [--token1 TOKEN] [--token2 TOKEN]

Ports are auto-detected by scanning 30001-30020.
"""

import socket
import json
import sys
import time
import argparse

PORT_RANGE = range(30001, 30021)


def send_rpc(sock, method, params=None, id=1):
    """Send a JSON-RPC 2.0 request and return the parsed response."""
    request = {"jsonrpc": "2.0", "method": method, "id": id}
    if params is not None:
        request["params"] = params
    payload = json.dumps(request) + "\n"
    sock.sendall(payload.encode("utf-8"))
    time.sleep(0.15)
    data = sock.recv(8192)
    try:
        return json.loads(data.decode("utf-8").strip())
    except json.JSONDecodeError:
        return None


def discover_instances():
    """Find all listening MCP server ports in the scan range."""
    found = []
    for port in PORT_RANGE:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.3)
            s.connect(("127.0.0.1", port))
            s.close()
            found.append(port)
        except (ConnectionRefusedError, OSError):
            continue
    return found


def connect(port):
    """Open a connection to the given port."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5.0)
    sock.connect(("127.0.0.1", port))
    return sock


def authenticate(sock, token, port):
    """Authenticate and return instance identity dict or None."""
    r = send_rpc(sock, "initialize", {"bearer_token": token}, id=1)
    if r and "result" in r:
        result = r["result"] if isinstance(r["result"], dict) else json.loads(r["result"])
        return result
    return None


def run_tests(ports, token1=None, token2=None):
    print("=" * 60)
    print("MorphSnap Multi-Instance Test")
    print("=" * 60)

    if len(ports) < 2:
        print(f"\n[ERROR] Need at least 2 instances, found {len(ports)}: {ports}")
        print("Load two instances of MorphSnap in FL Studio.")
        return False

    port_a, port_b = ports[0], ports[1]
    print(f"\nInstance A: port {port_a}")
    print(f"Instance B: port {port_b}")

    passed = 0
    failed = 0

    # ─── Test 1: Ports are distinct ───
    if port_a != port_b:
        passed += 1
        print("\n[PASS] Instances use distinct ports")
    else:
        failed += 1
        print("\n[FAIL] Instances share the same port!")

    # ─── Test 2: Unauthenticated requests rejected on both ───
    for label, port in [("A", port_a), ("B", port_b)]:
        sock = connect(port)
        r = send_rpc(sock, "get_plugin_info", id=100)
        sock.close()
        if r and "error" in r:
            passed += 1
            print(f"[PASS] Instance {label} (port {port}) rejects unauthenticated requests")
        else:
            failed += 1
            print(f"[FAIL] Instance {label} (port {port}) accepted unauthenticated request!")

    # If tokens provided, run deeper authentication tests
    if not token1 or not token2:
        print("\n[SKIP] Token-dependent tests (provide --token1 and --token2)")
        print(f"\nResults: {passed} passed, {failed} failed")
        return failed == 0

    # ─── Test 3: Authenticate instance A ───
    sock_a = connect(port_a)
    identity_a = authenticate(sock_a, token1, port_a)
    if identity_a:
        passed += 1
        print(f"\n[PASS] Instance A authenticated")
        print(f"       ID:        {identity_a.get('instanceId')}")
        print(f"       MorphCode: {identity_a.get('morphCode')}")
        print(f"       Port:      {identity_a.get('port')}")
    else:
        failed += 1
        print(f"\n[FAIL] Instance A authentication failed with provided token")
        sock_a.close()
        return False

    # ─── Test 4: Authenticate instance B ───
    sock_b = connect(port_b)
    identity_b = authenticate(sock_b, token2, port_b)
    if identity_b:
        passed += 1
        print(f"\n[PASS] Instance B authenticated")
        print(f"       ID:        {identity_b.get('instanceId')}")
        print(f"       MorphCode: {identity_b.get('morphCode')}")
        print(f"       Port:      {identity_b.get('port')}")
    else:
        failed += 1
        print(f"\n[FAIL] Instance B authentication failed with provided token")
        sock_a.close()
        sock_b.close()
        return False

    # ─── Test 5: Instance IDs are distinct ───
    if identity_a["instanceId"] != identity_b["instanceId"]:
        passed += 1
        print("\n[PASS] Instance IDs are distinct")
    else:
        failed += 1
        print("\n[FAIL] Instance IDs are identical!")

    # ─── Test 6: Morph codes are distinct ───
    if identity_a["morphCode"] != identity_b["morphCode"]:
        passed += 1
        print("[PASS] Morph codes are distinct")
    else:
        failed += 1
        print("[FAIL] Morph codes are identical!")

    # ─── Test 7: Each sees the other via list_instances ───
    r = send_rpc(sock_a, "list_instances", id=20)
    if r and "result" in r:
        result_str = str(r["result"])
        if identity_b["instanceId"][:8] in result_str or str(port_b) in result_str:
            passed += 1
            print("[PASS] Instance A can see Instance B via list_instances")
        else:
            failed += 1
            print("[FAIL] Instance A cannot see Instance B in list_instances")
    else:
        failed += 1
        print("[FAIL] list_instances failed on Instance A")

    # ─── Test 8: Cross-authentication must fail ───
    # Try to use token1 on port_b
    sock_cross = connect(port_b)
    r = send_rpc(sock_cross, "initialize", {"bearer_token": token1}, id=200)
    sock_cross.close()
    if r and "error" in r:
        passed += 1
        print("[PASS] Cross-auth rejected (token A on Instance B)")
    else:
        failed += 1
        print("[FAIL] Cross-auth succeeded (token A on Instance B)!")

    # Try to use token2 on port_a
    sock_cross = connect(port_a)
    r = send_rpc(sock_cross, "initialize", {"bearer_token": token2}, id=201)
    sock_cross.close()
    if r and "error" in r:
        passed += 1
        print("[PASS] Cross-auth rejected (token B on Instance A)")
    else:
        failed += 1
        print("[FAIL] Cross-auth succeeded (token B on Instance A)!")

    # ─── Test 9: Authenticated tool call on each instance ───
    for label, sock, port in [("A", sock_a, port_a), ("B", sock_b, port_b)]:
        r = send_rpc(sock, "get_instance_info", id=30)
        if r and "result" in r:
            passed += 1
            print(f"[PASS] get_instance_info works on Instance {label}")
        else:
            failed += 1
            print(f"[FAIL] get_instance_info failed on Instance {label}")

    sock_a.close()
    sock_b.close()

    # Summary
    print("\n" + "=" * 60)
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    print("=" * 60)
    return failed == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MorphSnap Multi-Instance Test")
    parser.add_argument("--token1", type=str, default=None,
                        help="Bearer token for instance A")
    parser.add_argument("--token2", type=str, default=None,
                        help="Bearer token for instance B")
    args = parser.parse_args()

    print("Discovering MCP instances...")
    ports = discover_instances()
    print(f"Found {len(ports)} instance(s) on ports: {ports}")

    success = run_tests(ports, args.token1, args.token2)
    sys.exit(0 if success else 1)
