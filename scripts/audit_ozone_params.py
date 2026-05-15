#!/usr/bin/env python3
"""
audit_ozone_params.py — Ozone 11 Parameter Audit Script
========================================================
Usage:
    1. Load Ozone 11 as the hosted plugin in More-Phi (any DAW).
    2. Ensure More-Phi's MCP server is running (default port 30001).
    3. Run:  python scripts/audit_ozone_params.py --port 30001 --token <bearer>
    4. Output: ozone11_params.json (raw dump) + ozone11_param_map.json (annotated map)

The annotated map identifies candidate parameter IDs for each mastering stage
by matching known Ozone 11 parameter name patterns.
"""

import socket
import json
import argparse
import re
import sys
from typing import Optional

# ── MCP helpers ──────────────────────────────────────────────────────────────

def mcp_request(sock: socket.socket, method: str, params: dict, req_id: int) -> dict:
    payload = json.dumps({
        "jsonrpc": "2.0",
        "method": "tools/call",
        "params": {"name": method, "arguments": params, "token": params.get("token", "")},
        "id": req_id,
    })
    sock.sendall((payload + "\n").encode())
    data = b""
    while b"\n" not in data:
        chunk = sock.recv(65536)
        if not chunk:
            break
        data += chunk
    return json.loads(data.split(b"\n")[0])


def list_parameters(host: str, port: int, token: Optional[str]) -> list:
    with socket.create_connection((host, port), timeout=10) as s:
        params: dict = {}
        if token:
            params["token"] = token
        resp = mcp_request(s, "list_parameters", params, 1)
    result = resp.get("result")
    if result is None:
        raise RuntimeError(f"MCP error: {resp}")
    # result may be a JSON string (More-Phi wraps as string) or list
    if isinstance(result, str):
        result = json.loads(result)
    # More-Phi returns {"params": [...]} or just [...]
    if isinstance(result, dict) and "params" in result:
        return result["params"]
    if isinstance(result, list):
        return result
    raise RuntimeError(f"Unexpected result shape: {type(result)}")


# ── Parameter matching patterns ───────────────────────────────────────────────
#
# These patterns are based on iZotope Ozone 11 Advanced parameter naming
# conventions. Adjust if your version differs.

EQ_PATTERNS = {
    "freq":    re.compile(r"EQ.*Band\s*(\d+).*Freq(?:uency)?", re.I),
    "gain":    re.compile(r"EQ.*Band\s*(\d+).*Gain", re.I),
    "q":       re.compile(r"EQ.*Band\s*(\d+).*[QBandwidth]", re.I),
    "type":    re.compile(r"EQ.*Band\s*(\d+).*(?:Type|Shape|Filter)", re.I),
    "enabled": re.compile(r"EQ.*Band\s*(\d+).*(?:Enabled?|On|Active)", re.I),
}

DYNAMICS_PATTERNS = {
    "threshold": re.compile(r"Dynamics.*Threshold", re.I),
    "ratio":     re.compile(r"Dynamics.*Ratio", re.I),
    "attack":    re.compile(r"Dynamics.*Attack", re.I),
    "release":   re.compile(r"Dynamics.*Release", re.I),
}

IMAGER_PATTERNS = {
    "width_sub":  re.compile(r"Imager.*(?:Sub|Low\s*Bass|Band\s*1).*Width", re.I),
    "width_low":  re.compile(r"Imager.*(?:Low|Bass|Band\s*2).*Width", re.I),
    "width_mid":  re.compile(r"Imager.*(?:Mid|Band\s*3).*Width", re.I),
    "width_high": re.compile(r"Imager.*(?:High|Air|Treble|Band\s*4).*Width", re.I),
}

MAXIMIZER_PATTERNS = {
    "output_level": re.compile(r"Maximizer.*(?:Output\s*Level|Threshold|Target)", re.I),
    "ceiling":      re.compile(r"Maximizer.*(?:Ceiling|True\s*Peak|TP)", re.I),
}


def annotate_params(params: list) -> dict:
    """Match parameter names to mastering-stage slots."""
    eq_bands: dict = {i: {} for i in range(1, 9)}
    dynamics: dict = {}
    imager: dict = {}
    maximizer: dict = {}
    unmatched: list = []

    for p in params:
        pid   = p["id"]
        name  = p["name"]
        matched = False

        # EQ bands
        for field, pattern in EQ_PATTERNS.items():
            m = pattern.search(name)
            if m:
                band_num = int(m.group(1)) if m.lastindex else 0
                if 1 <= band_num <= 8:
                    eq_bands[band_num][field] = {"id": pid, "name": name, "value": p.get("value")}
                    matched = True
                    break

        if matched:
            continue

        # Dynamics
        for field, pattern in DYNAMICS_PATTERNS.items():
            if pattern.search(name):
                if field not in dynamics:
                    dynamics[field] = {"id": pid, "name": name, "value": p.get("value")}
                matched = True
                break

        if matched:
            continue

        # Imager
        for field, pattern in IMAGER_PATTERNS.items():
            if pattern.search(name):
                if field not in imager:
                    imager[field] = {"id": pid, "name": name, "value": p.get("value")}
                matched = True
                break

        if matched:
            continue

        # Maximizer
        for field, pattern in MAXIMIZER_PATTERNS.items():
            if pattern.search(name):
                if field not in maximizer:
                    maximizer[field] = {"id": pid, "name": name, "value": p.get("value")}
                matched = True
                break

        if not matched:
            unmatched.append({"id": pid, "name": name})

    return {
        "eq_bands": eq_bands,
        "dynamics": dynamics,
        "imager": imager,
        "maximizer": maximizer,
        "unmatched_count": len(unmatched),
        "total": len(params),
    }


def main():
    parser = argparse.ArgumentParser(description="Audit Ozone 11 parameters via More-Phi MCP")
    parser.add_argument("--host",  default="127.0.0.1")
    parser.add_argument("--port",  type=int, default=30001)
    parser.add_argument("--token", default="", help="MCP bearer token (if required)")
    parser.add_argument("--out-raw",  default="ozone11_params.json")
    parser.add_argument("--out-map",  default="ozone11_param_map.json")
    args = parser.parse_args()

    print(f"Connecting to More-Phi MCP at {args.host}:{args.port} ...")
    try:
        params = list_parameters(args.host, args.port, args.token or None)
    except Exception as e:
        print(f"ERROR: {e}", file=sys.stderr)
        print("Ensure More-Phi is running with Ozone 11 as the hosted plugin.", file=sys.stderr)
        sys.exit(1)

    print(f"Retrieved {len(params)} parameters.")

    # Save raw dump (same format as neutron5_params.json)
    raw = {"id": 1, "jsonrpc": "2.0", "result": params}
    with open(args.out_raw, "w", encoding="utf-8") as f:
        json.dump(raw, f, indent=2)
    print(f"Raw dump saved → {args.out_raw}")

    # Save annotated map
    annotation = annotate_params(params)
    with open(args.out_map, "w", encoding="utf-8") as f:
        json.dump(annotation, f, indent=2)
    print(f"Annotated map saved → {args.out_map}")

    # Summary
    eq_complete = sum(1 for b in annotation["eq_bands"].values() if len(b) >= 3)
    print(f"\nAnnotation summary:")
    print(f"  EQ bands with ≥3 fields matched: {eq_complete}/8")
    print(f"  Dynamics fields matched: {len(annotation['dynamics'])}/4")
    print(f"  Imager width fields matched: {len(annotation['imager'])}/4")
    print(f"  Maximizer fields matched: {len(annotation['maximizer'])}/2")
    print(f"  Unmatched: {annotation['unmatched_count']}")
    if eq_complete < 6:
        print("\nWARNING: Few EQ bands matched. Check EQ_PATTERNS in this script against actual names.")


if __name__ == "__main__":
    main()
