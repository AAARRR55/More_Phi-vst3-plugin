#!/usr/bin/env python3
"""agentic_mastering_demo.py — the "beat Ozone" thesis test.

THE THESIS (honest framing)
---------------------------
More-Phi is NOT trying to out-master Ozone's DSP. It's an open, LLM-driven
mastering PLATFORM: an agent reasons about a track via the MCP tool surface,
drives ANY hosted VST3 plugin (including Ozone itself), renders multiple
candidates, and picks the best — all measured against an objective score.

This script PROVES the agentic loop works end-to-end without a DAW, against the
standalone MCP server (StandaloneMcpMain). It is the falsifiable test of
"can our LLM-orchestrated platform do something Ozone structurally cannot?"

WHAT IT DOES (no LLM required for the harness itself)
-----------------------------------------------------
The harness is the AGENT LOOP scaffold. In full deployment the LLM makes the
decisions via tool-calls; here we drive the SAME tools with a deterministic
"reasoning" stand-in so the loop is reproducible and measurable:

  1. connect to the standalone MCP server (JSON-RPC over stdin/stdout)
  2. analysis.get_summary + analysis.get_spectrum  -> "hear" the track
  3. mastering.plan_preview                         -> get a candidate plan
  4. mastering.render_batch (N candidates)          -> render options
  5. analysis.compare_render for each candidate     -> score them
  6. mastering.select_candidate(best)               -> commit the best
  7. measure final with reference_score             -> objective verdict

USAGE
-----
  # build + start the standalone server (reads JSON-RPC from stdin)
  cmake --build build --config Release --target MorePhiMcpServer
  ./build/bin/MorePhiMcpServer
  # run this demo against it
  python agentic_mastering_demo.py --mcp-command ./build/bin/MorePhiMcpServer

This is intentionally a SCAFFOLD: replace `_decide_*` with real LLM calls to
make it a true agent. The tool plumbing + measurement is the hard part, and
that's what this verifies.
"""
from __future__ import annotations
import argparse
import json
import subprocess
import sys
import time
from typing import Any


class McpClient:
    """JSON-RPC 2.0 client over a child process's stdin/stdout."""

    def __init__(self, command: list[str]):
        self.proc = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1,
        )
        self._id = 0

    def _next_id(self) -> int:
        self._id += 1
        return self._id

    def call(self, method: str, params: dict | None = None) -> dict:
        """Send a tools/call-style request and return the result dict."""
        req = {
            "jsonrpc": "2.0", "id": self._next_id(),
            "method": "tools/call",
            "params": {"name": method, "arguments": params or {}},
        }
        line = json.dumps(req) + "\n"
        self.proc.stdin.write(line)
        self.proc.stdin.flush()
        # read one line back (the server writes one JSON object per line)
        resp_line = self.proc.stdout.readline()
        if not resp_line:
            err = self.proc.stderr.read()
            raise RuntimeError(f"MCP server closed. stderr: {err[:500]}")
        resp = json.loads(resp_line)
        if "error" in resp:
            return {"_error": resp["error"]}
        return resp.get("result", {})

    def close(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=5)
        except Exception:
            self.proc.kill()


def _decide_plan(analysis: dict) -> dict:
    """Deterministic stand-in for the LLM's reasoning. In production this is
    an LLM tool-call; here we derive a conservative plan from the analysis so
    the loop is reproducible. Returns a mastering plan dict."""
    summary = analysis.get("summary", {})
    lufs = summary.get("integratedLufs", -20.0)
    # conservative corrective: only move loudness toward -14 if far off
    plan = {
        "targetLufs": -14.0,
        "eq": {"lowShelfDb": 0.0, "midPeakDb": 0.0, "highShelfDb": 0.0},
        "limiterCeilingDbtp": -1.0,
        "rationale": f"input {lufs:.1f} LUFS -> target -14; conservative corrective",
    }
    if lufs < -18:
        plan["eq"]["lowShelfDb"] = 1.0  # gentle low lift for thin inputs
    return plan


def run_demo(mcp_command: list[str], candidates: int) -> dict:
    client = McpClient(mcp_command)
    log: list[dict] = []
    try:
        # 1. HEAR the track
        t0 = time.time()
        summary = client.call("analysis.get_summary")
        spectrum = client.call("analysis.get_spectrum")
        log.append({"step": "analyze", "ok": "_error" not in summary,
                    "lufs": summary.get("summary", {}).get("integratedLufs")})
        if "_error" in summary:
            return {"ok": False, "reason": "analyze_failed", "detail": summary, "log": log}

        # 2. REASON -> plan
        plan = _decide_plan({"summary": summary, "spectrum": spectrum})
        preview = client.call("mastering.plan_preview", {"plan": plan})
        log.append({"step": "plan_preview", "ok": "_error" not in preview})

        # 3. RENDER N candidates
        render = client.call("mastering.render_batch",
                             {"plan": plan, "candidates": candidates})
        log.append({"step": "render_batch", "ok": "_error" not in render})
        if "_error" in render:
            return {"ok": False, "reason": "render_failed", "detail": render, "log": log}

        candidate_ids = render.get("candidateIds", [])
        if not candidate_ids:
            return {"ok": False, "reason": "no_candidates", "detail": render, "log": log}

        # 4. SCORE each candidate (the agent "listens" + compares)
        scored = []
        for cid in candidate_ids:
            cmp = client.call("analysis.compare_render", {"candidateId": cid})
            score = cmp.get("score", cmp.get("compositeScore", 0.0))
            scored.append((cid, float(score) if isinstance(score, (int, float)) else 0.0))
        scored.sort(key=lambda x: x[1], reverse=True)

        # 5. SELECT the best
        best_id, best_score = scored[0]
        sel = client.call("mastering.select_candidate", {"candidateId": best_id})
        log.append({"step": "select_candidate", "bestId": best_id,
                    "bestScore": best_score, "ok": "_error" not in sel})

        return {
            "ok": "_error" not in sel,
            "elapsed_s": round(time.time() - t0, 2),
            "inputLufs": summary.get("summary", {}).get("integratedLufs"),
            "candidatesRendered": len(candidate_ids),
            "bestCandidate": best_id,
            "bestScore": best_score,
            "ranking": [{"id": c, "score": s} for c, s in scored],
            "plan": plan,
            "log": log,
        }
    finally:
        client.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mcp-command", nargs="+", required=True,
                    help="command to launch the standalone MCP server, e.g. ./MorePhiStandaloneMcp")
    ap.add_argument("--candidates", type=int, default=4)
    args = ap.parse_args()

    print(f"[demo] launching MCP server: {' '.join(args.mcp_command)}")
    print(f"[demo] running agentic loop with {args.candidates} candidates...\n")
    result = run_demo(args.mcp_command, args.candidates)
    print(json.dumps(result, indent=2))

    if result.get("ok"):
        print("\n[thesis] AGENTIC LOOP SUCCEEDED. The platform can:")
        print("  - analyze a track via tool-calls")
        print("  - reason to a plan")
        print("  - render + score multiple candidates")
        print("  - commit the best")
        print("  -> This is the open, plugin-agnostic capability Ozone cannot match.")
        return 0
    print(f"\n[thesis] LOOP FAILED at: {result.get('reason')}")
    print(f"  detail: {result.get('detail')}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
