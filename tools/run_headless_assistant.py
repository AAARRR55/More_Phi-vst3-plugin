#!/usr/bin/env python3
"""
Headless Master Assistant lab driver.

Launches a More-Phi-OWNED host process (MorePhiMcpServer, which loads Ozone via
OZONE_VST3_PATH into its own PluginHostManager), attaches Frida to THAT pid only,
loads tools/ozone_headless_assistant.js, renders audio through Ozone via the
existing ozone_get_parameters / render path, and -- under an explicit,
multi-part gate -- calls the trigger's vtable slot and diffs Ozone params
before/after.

RED LINES (docs/OZONE_IPC_RESEARCH_METHODOLOGY.md):
  - NEVER attach to FL64.exe, iZotope.exe, PACE/iLok, or any vendor process.
    This driver attaches ONLY to the MorePhiMcpServer subprocess it spawns.
  - The internal call is NOT executed by default. The default mode is DRY-RUN:
    resolve the trigger, log the address + disassembly snapshot, render audio,
    capture a param baseline, and return WITHOUT calling. The call only happens
    when ALL of these hold:
        (1) env  MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1   (the existing sec-9 gate)
        (2) env  OZONE_HEADLESS_INVOKE=1               (the private-call opt-in)
        (3) CLI  --i-understand-the-risk               (explicit per-run consent)
        (4) in-file ENABLE_PRIVATE_CALL=true in ozone_headless_assistant.js
        (5) rpc.invoke() opts.enable=true              (defense-in-depth per call)
  - It does NOT touch PACE/iLok, licensing, anti-tamper, or integrity-check
    logic. It never calls or instruments the applier (0xEAD930).

USAGE (dry-run, the safe default):
    py tools/run_headless_assistant.py \\
        --audio path/to/track.wav \\
        --out-dir tools/live_captures/headless

USAGE (gated live call -- only run this deliberately):
    set MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1
    set OZONE_HEADLESS_INVOKE=1
    py tools/run_headless_assistant.py --audio track.wav \\
        --i-understand-the-risk --controller-this 0x... --inout-ctx 0x... \\
        --opts-out 0x... --out-dir tools/live_captures/headless

The driver refuses to invoke without --i-understand-the-risk even if the env
gates are set.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import os
import pathlib
import re
import shlex
import subprocess
import sys
import time
from typing import Any


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
FRIDA_SCRIPT = pathlib.Path(__file__).resolve().parent / "ozone_headless_assistant.js"

# JSON-RPC tool names exposed by StandaloneMcpServer (src/AI/StandaloneMcp/
# StandaloneMcpServer.cpp). We drive the host over stdio JSON-RPC 2.0.
TOOL_GET_PARAMS = "ozone_get_parameters"
TOOL_SET_PARAM = "ozone_set_parameter"
TOOL_RUN_ASSISTANT = "ozone_run_master_assistant"  # the SANCTIONED public path (diff fallback)

DEFAULT_OZONE_VST3 = r"C:\Program Files\Common Files\VST3\iZotope\Ozone Pro.vst3"
DEFAULT_MCP_SERVER_CANDIDATES = [
    REPO_ROOT / "build" / "Release" / "MorePhiMcpServer.exe",
    REPO_ROOT / "build" / "Release" / "MorePhiMcpServer",
    REPO_ROOT / "build" / "windows-msvc-release" / "Release" / "MorePhiMcpServer.exe",
    REPO_ROOT / "build" / "windows-msvc-safe" / "Release" / "MorePhiMcpServer.exe",
]

# The driver-side gate names. The in-file flag ENABLE_PRIVATE_CALL lives in
# ozone_headless_assistant.js; this driver also enforces its own two env gates +
# the explicit CLI consent flag before it will even post rpc.invoke().
ENV_GATE_IPC_WRITE = "MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE"
ENV_GATE_PRIVATE_CALL = "OZONE_HEADLESS_INVOKE"


# ---------------------------------------------------------------------------
# Errors
# ---------------------------------------------------------------------------

class GateError(RuntimeError):
    """Raised when a safety gate is not satisfied. Never retried automatically."""


class HostError(RuntimeError):
    """Raised when the MCP host fails to load Ozone or respond."""


# ---------------------------------------------------------------------------
# JSON-RPC over stdio to MorePhiMcpServer
# ---------------------------------------------------------------------------

class McpStdioClient:
    """Minimal JSON-RPC 2.0 client over the MCP server's stdio."""

    def __init__(self, proc: subprocess.Popen[str]):
        self._proc = proc
        self._next_id = 1

    def _read_message(self) -> dict[str, Any]:
        assert self._proc.stdout is not None
        line = self._proc.stdout.readline()
        if not line:
            raise HostError("MCP host closed stdout (likely crashed on Ozone load). "
                            "Check OZONE_VST3_PATH and that the build is current.")
        line = line.strip()
        if not line:
            return self._read_message()
        try:
            return json.loads(line)
        except json.JSONDecodeError as exc:
            raise HostError(f"MCP host emitted non-JSON on stdout: {line[:200]!r}") from exc

    def call(self, method: str, params: dict[str, Any] | None = None,
             timeout: float = 60.0) -> dict[str, Any]:
        req_id = self._next_id
        self._next_id += 1
        request = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            request["params"] = params
        assert self._proc.stdin is not None
        self._proc.stdin.write(json.dumps(request) + "\n")
        self._proc.stdin.flush()

        deadline = time.time() + timeout
        while time.time() < deadline:
            msg = self._read_message()
            # Skip notifications / server-originated messages without an id.
            if msg.get("id") == req_id:
                if "error" in msg:
                    err = msg["error"]
                    raise HostError(f"JSON-RPC error calling {method}: {err}")
                return msg.get("result", {})
            # else: keep reading until we see our reply
        raise HostError(f"Timed out waiting for reply to {method} (id={req_id}).")

    def call_tool(self, name: str, arguments: dict[str, Any] | None = None,
                  timeout: float = 120.0) -> dict[str, Any]:
        return self.call("tools/call", {"name": name, "arguments": arguments or {}},
                         timeout=timeout)

    def initialize(self) -> dict[str, Any]:
        return self.call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "run_headless_assistant", "version": "1.0"},
        })

    def close(self) -> None:
        try:
            if self._proc.stdin is not None:
                self._proc.stdin.close()
        except Exception:
            pass
        try:
            self._proc.terminate()
            self._proc.wait(timeout=5)
        except Exception:
            try:
                self._proc.kill()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Host launch
# ---------------------------------------------------------------------------

def find_mcp_server(explicit: str | None) -> pathlib.Path:
    if explicit:
        p = pathlib.Path(explicit)
        if not p.exists():
            raise HostError(f"Explicit MCP server not found: {p}")
        return p
    for cand in DEFAULT_MCP_SERVER_CANDIDATES:
        if cand.exists():
            return cand
    raise HostError(
        "Could not find a built MorePhiMcpServer. Build it first:\n"
        "    cmake --build build --config Release --target MorePhiMcpServer\n"
        "or pass --mcp-server /path/to/MorePhiMcpServer[.exe]."
    )


def launch_host(server_path: pathlib.Path, ozone_vst3: str,
                sample_rate: int, block_size: int) -> tuple[subprocess.Popen[str], int]:
    env = os.environ.copy()
    env["OZONE_VST3_PATH"] = ozone_vst3
    env.setdefault("OZONE_SAMPLE_RATE", str(sample_rate))
    env.setdefault("OZONE_BLOCK_SIZE", str(block_size))
    # Do NOT set the write/invoke gates here; they are checked explicitly below.
    env.pop(ENV_GATE_IPC_WRITE, None)
    env.pop(ENV_GATE_PRIVATE_CALL, None)

    # The MCP server is a stdio JSON-RPC process. We capture its stderr to a
    # file so we can diagnose Ozone-load failures without losing it.
    proc = subprocess.Popen(
        [str(server_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
        env=env,
    )
    return proc, proc.pid


# ---------------------------------------------------------------------------
# Frida
# ---------------------------------------------------------------------------

def attach_frida(pid: int):
    try:
        import frida  # type: ignore
    except Exception as exc:
        raise RuntimeError(
            f"frida python package not available: {exc}. Install with: py -m pip install frida"
        ) from exc

    # SAFETY: refuse to attach if the pid is not a More-Phi-owned process we
    # spawned. We can't fully trust the name (a vendor process could in theory
    # be named MorePhiMcpServer), but we can refuse well-known vendor names.
    try:
        device = frida.get_local_device()
        proc_info = device.get_process(pid)
        pname = (proc_info.name or "").lower()
        forbidden = ("fl64", "fl32", "flstudio", "izotope", "ozone", "pace",
                     "ilok", "reaper", "ableton", "cubase", "protools", "studio one")
        for bad in forbidden:
            if bad in pname:
                raise GateError(
                    f"REFUSING to attach Frida: pid {pid} name {proc_info.name!r} "
                    f"matches a forbidden vendor/DAW pattern ({bad!r}). This driver "
                    f"attaches ONLY to MorePhiMcpServer subprocesses. Aborting."
                )
    except GateError:
        raise
    except Exception:
        # Best-effort name check; if it fails, continue (we spawned the pid ourselves).
        pass

    session = device.attach(pid)
    return frida, session


def load_script(frida_mod, session, script_path: pathlib.Path,
                on_message) -> Any:
    src = script_path.read_text(encoding="utf-8")
    script = session.create_script(src)
    script.on("message", on_message)
    script.load()
    return script


# ---------------------------------------------------------------------------
# Gate enforcement
# ---------------------------------------------------------------------------

def check_invoke_gates(args: argparse.Namespace) -> None:
    """Raise GateError unless ALL gates for a live invoke are satisfied.

    Note: the in-file ENABLE_PRIVATE_CALL flag in ozone_headless_assistant.js is
    checked separately via rpc.gateState() after load; we surface it in the report.
    """
    missing: list[str] = []

    if os.environ.get(ENV_GATE_IPC_WRITE) != "1":
        missing.append(f"env {ENV_GATE_IPC_WRITE}=1 (the existing sec-9 IPC write gate)")
    if os.environ.get(ENV_GATE_PRIVATE_CALL) != "1":
        missing.append(f"env {ENV_GATE_PRIVATE_CALL}=1 (the private-call opt-in)")
    if not args.i_understand_the_risk:
        missing.append("CLI --i-understand-the-risk (explicit per-run consent)")
    if not args.controller_this:
        missing.append("--controller-this 0x... (live controller object base; "
                       "never synthesized)")
    if not args.inout_ctx:
        missing.append("--inout-ctx 0x... (RDX in/out context)")
    if not args.opts_out:
        missing.append("--opts-out 0x... (R8 output/options struct)")

    if missing:
        msg = (
            "GATED: refusing to invoke the Master Assistant trigger. The live "
            "internal call requires ALL of the following to be satisfied; missing:\n"
        )
        for m in missing:
            msg += f"    - {m}\n"
        msg += (
            "\nThe driver runs in DRY-RUN mode by default (resolve + observe + "
            "param baseline, NO call). If you have a verified live controller "
            "object and understand the PACE/anti-tamper caveat in "
            "docs/OZONE_HEADLESS_ASSISTANT_RUNBOOK.md, set every gate and re-run."
        )
        raise GateError(msg)


# ---------------------------------------------------------------------------
# Param capture / diff
# ---------------------------------------------------------------------------

_PARAM_NAME_RE = re.compile(r"^[A-Za-z0-9_./+-]+$")


def capture_params(client: McpStdioClient) -> dict[str, dict[str, Any]]:
    """Return {stable_id_or_index: {index, value, name, text}} via ozone_get_parameters."""
    result = client.call_tool(TOOL_GET_PARAMS, {"include_values": True})
    # tools/call result shape: {"content": [{"type":"text","text": "<json>"}]}
    text = _extract_text(result)
    try:
        payload = json.loads(text)
    except json.JSONDecodeError:
        payload = {"raw": text}
    params = payload.get("parameters", []) if isinstance(payload, dict) else []
    out: dict[str, dict[str, Any]] = {}
    for p in params:
        key = p.get("stable_id") or str(p.get("index"))
        out[key] = {
            "index": p.get("index"),
            "value": p.get("value"),
            "text": p.get("text"),
            "name": p.get("name"),
        }
    return out


def _extract_text(result: dict[str, Any]) -> str:
    content = result.get("content")
    if isinstance(content, list):
        parts = []
        for c in content:
            if isinstance(c, dict) and c.get("type") == "text":
                parts.append(c.get("text", ""))
        return "\n".join(parts)
    if isinstance(content, str):
        return content
    return json.dumps(result)


def diff_params(before: dict[str, dict[str, Any]],
                after: dict[str, dict[str, Any]]) -> list[dict[str, Any]]:
    deltas: list[dict[str, Any]] = []
    keys = sorted(set(before) | set(after))
    for k in keys:
        b = before.get(k, {})
        a = after.get(k, {})
        bv = b.get("value")
        av = a.get("value")
        if bv != av:
            try:
                magnitude = abs(float(av) - float(bv)) if bv is not None and av is not None else None
            except (TypeError, ValueError):
                magnitude = None
            deltas.append({
                "key": k,
                "name": a.get("name") or b.get("name"),
                "before": bv,
                "after": av,
                "before_text": b.get("text"),
                "after_text": a.get("text"),
                "magnitude": magnitude,
            })
    return deltas


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def run_research_id() -> str:
    return "headless-assistant-" + _dt.datetime.now().strftime("%Y%m%d-%H%M%S")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Headless Master Assistant lab driver (DRY-RUN by default).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("--audio", required=True,
                    help="Audio file rendered through Ozone to arm the signal chain.")
    ap.add_argument("--out-dir", default="tools/live_captures/headless",
                    help="Where to write the JSONL + report.")
    ap.add_argument("--mcp-server", default=None,
                    help="Path to MorePhiMcpServer[.exe]. Auto-discovered if omitted.")
    ap.add_argument("--ozone-vst3", default=os.environ.get("OZONE_VST3_PATH",
                                                            DEFAULT_OZONE_VST3),
                    help="Ozone Pro VST3 bundle path (default: the system install).")
    ap.add_argument("--analysis-seconds", type=float, default=30.0)
    ap.add_argument("--sample-rate", type=int, default=44100)
    ap.add_argument("--block-size", type=int, default=512)

    # --- Gated live call options ---
    ap.add_argument("--i-understand-the-risk", action="store_true",
                    help="Explicit per-run consent for the internal call. "
                         "Required (along with both env gates) to invoke.")
    ap.add_argument("--controller-this", default=None,
                    help="Live controller object base pointer (hex). NEVER synthesized.")
    ap.add_argument("--inout-ctx", default=None, help="RDX in/out context pointer (hex).")
    ap.add_argument("--opts-out", default=None, help="R8 output/options struct pointer (hex).")
    ap.add_argument("--no-render", action="store_true",
                    help="Skip the audio render step (debugging only).")
    args = ap.parse_args()

    out_dir = pathlib.Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    research_id = run_research_id()
    events_path = out_dir / f"{research_id}_events.jsonl"
    report_path = out_dir / f"{research_id}_report.json"

    events: list[dict[str, Any]] = []

    def record(stage: str, **kw: Any) -> None:
        ev = {"ts": _dt.datetime.now().isoformat(), "stage": stage, **kw}
        events.append(ev)
        with events_path.open("a", encoding="utf-8") as fp:
            fp.write(json.dumps(ev) + "\n")
        print(f"[{stage}] " + json.dumps(kw, default=str)[:240])

    # --- 0. resolve the trigger RVA offline first (sanity, no attach) ---
    record("start", research_id=research_id,
           ozone_vst3=args.ozone_vst3, audio=args.audio,
           gated_mode=args.i_understand_the_risk)

    # --- 1. launch the More-Phi-owned host ---
    server_path = find_mcp_server(args.mcp_server)
    record("host_launch", server=str(server_path), ozone_vst3=args.ozone_vst3)
    proc, pid = launch_host(server_path, args.ozone_vst3,
                            args.sample_rate, args.block_size)
    client = McpStdioClient(proc)
    try:
        init = client.initialize()
        record("host_initialized", pid=pid, server_info=init.get("serverInfo"))

        # Confirm Ozone actually loaded.
        params_baseline_probe = capture_params(client)
        if not params_baseline_probe:
            stderr = ""
            try:
                stderr = proc.stderr.read() if proc.stderr else ""
            except Exception:
                pass
            raise HostError(
                "Host loaded but ozone_get_parameters returned no parameters. "
                "Ozone likely failed to load. MCP stderr:\n" + (stderr or "<empty>")
            )
        record("ozone_loaded", pid=pid, param_count=len(params_baseline_probe))

        # --- 2. attach Frida to OUR host pid, load the script ---
        frida_mod, session = attach_frida(pid)
        record("frida_attached", pid=pid)

        def on_message(message: dict[str, Any], data: Any) -> None:
            payload = message.get("payload", message) if message.get("type") == "send" else message
            if message.get("type") == "error":
                record("frida_script_error", error=message.get("stack") or message.get("description"))
                return
            record("frida_message", **payload if isinstance(payload, dict)
                   else {"msg": payload})

        script = load_script(frida_mod, session, FRIDA_SCRIPT, on_message)
        time.sleep(0.5)

        # --- 3. resolve the trigger (DRY-RUN safe: no call) ---
        gate_state = script.exports_sync.gate_state()
        record("gate_state", **gate_state)

        resolution = script.exports_sync.resolve()
        record("trigger_resolved", **resolution)

        # --- 4. capture param baseline BEFORE any render / invoke ---
        before = capture_params(client)
        record("params_before", count=len(before))

        # --- 5. render audio through Ozone to arm the signal chain ---
        if not args.no_render:
            render_args = {
                "input_audio_path": str(pathlib.Path(args.audio).resolve()),
                "analysis_seconds": args.analysis_seconds,
            }
            render_result = client.call_tool(TOOL_RUN_ASSISTANT, render_args)
            record("audio_rendered",
                   result_text=_extract_text(render_result)[:1000])

        params_after_render = capture_params(client)
        render_deltas = diff_params(before, params_after_render)
        record("params_after_render", count=len(params_after_render),
               changed_by_render=len(render_deltas))

        # --- 6. GATE CHECK for the live invoke ---
        try:
            check_invoke_gates(args)
        except GateError as exc:
            record("gate_blocked", mode="DRY-RUN", reason=str(exc))
            # Write the report and exit cleanly. This is the expected path.
            report = {
                "research_id": research_id,
                "mode": "DRY-RUN",
                "pid": pid,
                "gate_state": gate_state,
                "resolution": resolution,
                "param_count_before": len(before),
                "param_count_after_render": len(params_after_render),
                "changed_by_render": render_deltas[:50],
                "note": ("No internal call was made. The driver ran in DRY-RUN mode: "
                         "resolved the trigger, captured a param baseline, rendered "
                         "audio, and returned. To invoke, satisfy every gate listed "
                         "in the gate_blocked event and see "
                         "docs/OZONE_HEADLESS_ASSISTANT_RUNBOOK.md."),
            }
            report_path.write_text(json.dumps(report, indent=2, default=str),
                                   encoding="utf-8")
            print(f"\nDRY-RUN complete. Report: {report_path}")
            print("No internal call was made (did_not_execute=true).")
            return 0

        # --- 7. (GATED) invoke the trigger via the vtable thunk ---
        # If we reach here, every gate passed. Double-check the in-file flag too.
        if not gate_state.get("enable_private_call"):
            record("gate_blocked", mode="in-file-flag-false",
                   reason="ENABLE_PRIVATE_CALL is false in ozone_headless_assistant.js. "
                          "The operator must flip it true in-file to dispatch.")
            report = {
                "research_id": research_id, "mode": "DRY-RUN (in-file flag false)",
                "pid": pid, "gate_state": gate_state, "resolution": resolution,
                "note": "All driver gates passed but the in-file ENABLE_PRIVATE_CALL "
                        "flag is still false, so no call was made."
            }
            report_path.write_text(json.dumps(report, indent=2, default=str),
                                   encoding="utf-8")
            print(f"\nIn-file flag false; no call. Report: {report_path}")
            return 0

        record("invoke_dispatching",
               controller_this=args.controller_this,
               inout_ctx=args.inout_ctx,
               opts_out=args.opts_out)

        invoke_opts = {
            "controllerThis": args.controller_this,
            "inOutCtx": args.inout_ctx,
            "optsOrOut": args.opts_out,
            "enable": True,
        }
        invoke_result = script.exports_sync.invoke(invoke_opts)
        record("invoke_result", **invoke_result)

        # --- 8. read back param deltas ---
        time.sleep(2.0)  # let the analysis pipeline stage
        after = capture_params(client)
        deltas = diff_params(before, after)
        record("params_after_invoke", count=len(after),
               changed_by_invoke=len(deltas))

        report = {
            "research_id": research_id,
            "mode": "GATED-LIVE-CALL",
            "pid": pid,
            "gate_state": gate_state,
            "resolution": resolution,
            "invoke_result": invoke_result,
            "param_count_before": len(before),
            "param_count_after": len(after),
            "param_deltas": deltas,
            "note": ("Internal call dispatched through the vtable thunk. The verify "
                     "verdict notes 0xD58A20's precise semantics are NOT confirmed by "
                     "dynamic correlation; inspect param_deltas to see what (if "
                     "anything) the call actually mutated."),
        }
        report_path.write_text(json.dumps(report, indent=2, default=str),
                               encoding="utf-8")
        print(f"\nGated call complete. Report: {report_path}")
        return 0

    except GateError as exc:
        record("gate_error", message=str(exc))
        print(f"\nGATE ERROR:\n{exc}", file=sys.stderr)
        return 2
    except HostError as exc:
        record("host_error", message=str(exc))
        print(f"\nHOST ERROR:\n{exc}", file=sys.stderr)
        return 3
    except Exception as exc:
        record("fatal_error", message=str(exc))
        print(f"\nFATAL: {exc}", file=sys.stderr)
        raise
    finally:
        try:
            client.close()
        except Exception:
            pass
        # Final flush of events already happened incrementally.
        print(f"Events: {events_path}")


if __name__ == "__main__":
    raise SystemExit(main())
