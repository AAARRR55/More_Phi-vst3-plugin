"""
MorphSnap MCP Server Test Client (v2 — multi-instance aware)
Connects to the JSON-RPC 2.0 server on localhost (auto-detected port),
authenticates with a bearer token, and exercises all tool endpoints.

Usage:
  1. Load MorphSnap in FL Studio (it auto-starts the MCP server)
  2. Run: python test_mcp.py [--port PORT] [--token TOKEN]

If no --port/--token given, the script scans ports 30001-30020 for a
listening MCP server and attempts auth handshake.
"""

import socket
import json
import sys
import time
import argparse

DEFAULT_PORT_RANGE = range(30001, 30021)


def send_rpc(sock, method, params=None, id=1):
    """Send a JSON-RPC 2.0 request and return the parsed response."""
    request = {
        "jsonrpc": "2.0",
        "method": method,
        "id": id,
    }
    if params is not None:
        request["params"] = params

    payload = json.dumps(request) + "\n"
    print(f"\n>>> {method}")
    print(f"    Request:  {payload.strip()}")

    sock.sendall(payload.encode("utf-8"))
    time.sleep(0.15)

    data = sock.recv(8192)
    response = data.decode("utf-8").strip()
    print(f"    Response: {response}")

    try:
        return json.loads(response)
    except json.JSONDecodeError:
        print(f"    [ERROR] Could not parse response")
        return None


def find_instance(port_range=DEFAULT_PORT_RANGE):
    """Scan port range for a listening MCP server. Returns (port) or None."""
    for port in port_range:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(0.3)
            s.connect(("127.0.0.1", port))
            s.close()
            return port
        except (ConnectionRefusedError, OSError):
            continue
    return None


def run_tests(port, token=None):
    print("=" * 60)
    print("MorphSnap MCP Server Test Client (v2)")
    print(f"Connecting to 127.0.0.1:{port}...")
    print("=" * 60)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect(("127.0.0.1", port))
        print("Connected!")
    except ConnectionRefusedError:
        print(f"\n[ERROR] Could not connect to 127.0.0.1:{port}")
        print("Make sure MorphSnap is loaded in FL Studio.")
        sys.exit(1)

    passed = 0
    failed = 0

    # ─── Test 0: Unauthenticated request should be rejected ───
    print("\n--- Auth Tests ---")
    r = send_rpc(sock, "get_plugin_info", id=100)
    if r and "error" in r:
        passed += 1
        print("    [PASS] Unauthenticated request rejected")
    else:
        failed += 1
        print("    [FAIL] Unauthenticated request was NOT rejected")

    # ─── Test 1: Auth with wrong token should fail ───
    r = send_rpc(sock, "initialize", {"bearer_token": "wrong_token_123"}, id=101)
    if r and "error" in r:
        passed += 1
        print("    [PASS] Wrong bearer token rejected")
    else:
        failed += 1
        print("    [FAIL] Wrong bearer token was NOT rejected")

    # ─── Test 2: Auth with correct token should succeed ───
    if token:
        r = send_rpc(sock, "initialize", {"bearer_token": token}, id=102)
        if r and "result" in r:
            result = r["result"] if isinstance(r["result"], dict) else json.loads(r["result"])
            instance_id = result.get("instanceId", "")
            morph_code = result.get("morphCode", "")
            reported_port = result.get("port", 0)
            print(f"    Instance ID:  {instance_id}")
            print(f"    Morph Code:   {morph_code}")
            print(f"    Port:         {reported_port}")
            if instance_id and morph_code and reported_port == port:
                passed += 1
                print("    [PASS] Authentication succeeded with instance identity")
            else:
                failed += 1
                print("    [FAIL] Auth succeeded but identity incomplete")
        else:
            failed += 1
            print("    [FAIL] Authentication with correct token failed")
            print("    Remaining tests require auth. Aborting.")
            sock.close()
            return False
    else:
        print("    [SKIP] No --token provided, skipping auth + tool tests")
        sock.close()
        print(f"\nResults: {passed} passed, {failed} failed")
        return failed == 0

    # ─── Authenticated tool tests ───
    print("\n--- Tool Tests (Authenticated) ---")

    # Test 3: get_plugin_info
    r = send_rpc(sock, "get_plugin_info", id=1)
    if r and "result" in r:
        passed += 1
        print("    [PASS] get_plugin_info")
    else:
        failed += 1
        print("    [FAIL] get_plugin_info")

    # Test 4: get_instance_info
    r = send_rpc(sock, "get_instance_info", id=10)
    if r and "result" in r:
        passed += 1
        print("    [PASS] get_instance_info")
    else:
        failed += 1
        print("    [FAIL] get_instance_info")

    # Test 5: list_instances
    r = send_rpc(sock, "list_instances", id=11)
    if r and "result" in r:
        passed += 1
        print("    [PASS] list_instances")
    else:
        failed += 1
        print("    [FAIL] list_instances")

    # Test 6: get_morph_state
    r = send_rpc(sock, "get_morph_state", id=2)
    if r and "result" in r:
        passed += 1
        print("    [PASS] get_morph_state")
    else:
        failed += 1
        print("    [FAIL] get_morph_state")

    # Test 7: set_morph_position (XY)
    r = send_rpc(sock, "set_morph_position", {"x": 0.75, "y": 0.25}, id=3)
    if r and "result" in r:
        passed += 1
        print("    [PASS] set_morph_position (x=0.75, y=0.25)")
    else:
        failed += 1
        print("    [FAIL] set_morph_position")

    # Test 8: list_parameters
    r = send_rpc(sock, "list_parameters", {}, id=5)
    if r and "result" in r:
        passed += 1
        print("    [PASS] list_parameters")
    else:
        failed += 1
        print("    [FAIL] list_parameters")

    # Test 9: capture_snapshot
    r = send_rpc(sock, "capture_snapshot", {"slot": 0}, id=6)
    if r and "result" in r:
        passed += 1
        print("    [PASS] capture_snapshot (slot 0)")
    else:
        failed += 1
        print("    [FAIL] capture_snapshot")

    # Test 10: recall_snapshot
    r = send_rpc(sock, "recall_snapshot", {"slot": 0}, id=7)
    if r and "result" in r:
        passed += 1
        print("    [PASS] recall_snapshot (slot 0)")
    else:
        failed += 1
        print("    [FAIL] recall_snapshot")

    # Test 11: Unknown method
    r = send_rpc(sock, "nonexistent_tool", {}, id=9)
    if r and "result" in r:
        result_str = str(r["result"])
        if "error" in result_str.lower() or "unknown" in result_str.lower():
            passed += 1
            print("    [PASS] unknown method returns error")
        else:
            failed += 1
            print("    [FAIL] unknown method should return error")
    elif r and "error" in r:
        passed += 1
        print("    [PASS] unknown method returns error")
    else:
        failed += 1
        print("    [FAIL] unknown method")

    sock.close()

    # Summary
    print("\n" + "=" * 60)
    print(f"Results: {passed} passed, {failed} failed, {passed + failed} total")
    print("=" * 60)
    return failed == 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="MorphSnap MCP Test Client")
    parser.add_argument("--port", type=int, default=0,
                        help="Port to connect to (0 = auto-detect)")
    parser.add_argument("--token", type=str, default=None,
                        help="Bearer token for authentication")
    args = parser.parse_args()

    port = args.port
    if port == 0:
        print("Auto-detecting MCP server port...")
        port = find_instance()
        if port is None:
            print("[ERROR] No MCP server found on ports 30001-30020")
            print("Make sure MorphSnap is loaded in FL Studio.")
            sys.exit(1)
        print(f"Found MCP server on port {port}")

    success = run_tests(port, args.token)
    sys.exit(0 if success else 1)
