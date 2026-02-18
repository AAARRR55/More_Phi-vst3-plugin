"""
MorphSnap MCP Server Test Client
Connects to the JSON-RPC 2.0 server on localhost:30001
and exercises all 9 tool endpoints.

Usage:
  1. Load MorphSnap in FL Studio (it auto-starts the MCP server)
  2. Run: python test_mcp.py
"""

import socket
import json
import sys
import time

HOST = "127.0.0.1"
PORT = 30001

def send_rpc(sock, method, params=None, id=1):
    """Send a JSON-RPC 2.0 request and return the parsed response."""
    request = {
        "jsonrpc": "2.0",
        "method": method,
        "id": id,
    }
    if params is not None:
        request["params"] = params

    payload = json.dumps(request)
    print(f"\n>>> {method}")
    print(f"    Request:  {payload}")

    sock.sendall(payload.encode("utf-8"))
    time.sleep(0.1)  # Give server time to process

    data = sock.recv(8192)
    response = data.decode("utf-8")
    print(f"    Response: {response}")

    try:
        return json.loads(response)
    except json.JSONDecodeError:
        print(f"    [ERROR] Could not parse response")
        return None


def run_tests():
    print("=" * 60)
    print("MorphSnap MCP Server Test Client")
    print(f"Connecting to {HOST}:{PORT}...")
    print("=" * 60)

    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.settimeout(5.0)
        sock.connect((HOST, PORT))
        print("Connected!")
    except ConnectionRefusedError:
        print(f"\n[ERROR] Could not connect to {HOST}:{PORT}")
        print("Make sure MorphSnap is loaded in FL Studio.")
        print("The MCP server auto-starts on port 30001.")
        sys.exit(1)

    passed = 0
    failed = 0

    # Test 1: get_plugin_info
    r = send_rpc(sock, "get_plugin_info", id=1)
    if r and "result" in r:
        passed += 1
        print("    [PASS] get_plugin_info")
    else:
        failed += 1
        print("    [FAIL] get_plugin_info")

    # Test 2: get_morph_state
    r = send_rpc(sock, "get_morph_state", id=2)
    if r and "result" in r:
        passed += 1
        print("    [PASS] get_morph_state")
    else:
        failed += 1
        print("    [FAIL] get_morph_state")

    # Test 3: set_morph_position (XY)
    r = send_rpc(sock, "set_morph_position", {"x": 0.75, "y": 0.25}, id=3)
    if r and "result" in r:
        passed += 1
        print("    [PASS] set_morph_position (x=0.75, y=0.25)")
    else:
        failed += 1
        print("    [FAIL] set_morph_position")

    # Test 4: Verify morph state changed
    r = send_rpc(sock, "get_morph_state", id=4)
    if r and "result" in r:
        result = r["result"] if isinstance(r["result"], dict) else json.loads(r["result"])
        if abs(result.get("x", 0) - 0.75) < 0.01:
            passed += 1
            print("    [PASS] morph position verified (x ≈ 0.75)")
        else:
            failed += 1
            print(f"    [FAIL] morph position mismatch (x = {result.get('x')})")
    else:
        failed += 1
        print("    [FAIL] get_morph_state verification")

    # Test 5: list_parameters
    r = send_rpc(sock, "list_parameters", {}, id=5)
    if r and "result" in r:
        passed += 1
        print("    [PASS] list_parameters")
    else:
        failed += 1
        print("    [FAIL] list_parameters")

    # Test 6: capture_snapshot (slot 0)
    r = send_rpc(sock, "capture_snapshot", {"slot": 0}, id=6)
    if r and "result" in r:
        passed += 1
        print("    [PASS] capture_snapshot (slot 0)")
    else:
        failed += 1
        print("    [FAIL] capture_snapshot")

    # Test 7: recall_snapshot (slot 0)
    r = send_rpc(sock, "recall_snapshot", {"slot": 0}, id=7)
    if r and "result" in r:
        passed += 1
        print("    [PASS] recall_snapshot (slot 0)")
    else:
        failed += 1
        print("    [FAIL] recall_snapshot")

    # Test 8: set_morph_position (fader)
    r = send_rpc(sock, "set_morph_position", {"fader": 0.5}, id=8)
    if r and "result" in r:
        passed += 1
        print("    [PASS] set_morph_position (fader=0.5)")
    else:
        failed += 1
        print("    [FAIL] set_morph_position fader")

    # Test 9: Unknown method
    r = send_rpc(sock, "nonexistent_tool", {}, id=9)
    if r and "result" in r:
        result = r["result"] if isinstance(r["result"], dict) else json.loads(str(r["result"]))
        if "error" in str(result):
            passed += 1
            print("    [PASS] unknown method returns error")
        else:
            failed += 1
            print("    [FAIL] unknown method should return error")
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
    success = run_tests()
    sys.exit(0 if success else 1)
