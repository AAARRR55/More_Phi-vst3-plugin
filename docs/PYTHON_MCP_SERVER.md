# Python stdio MCP Server for More-Phi

This document describes the Python-based MCP server that exposes More-Phi's
hosted VST3 plugin through the Model Context Protocol (MCP) stdio transport.
It implements the architecture specified in the AI–MCP–VST3 Integration
Technical Specification (v1.0).

> **Note:** This Python stdio server is an alternative transport to the embedded C++
> `MCPServer` (TCP, localhost:30001). For the primary orchestration layer that coordinates
> the embedded MCP server with the multi-agent system, see `docs/ECOSYSTEM.md` and
> `src/AI/Orchestrator/AgentOrchestrator.h`.

## Overview

The AI host (Claude Desktop, local LLM runtime, etc.) spawns
`scripts/vst3-mcp-server/server.py` as a subprocess and communicates over
stdin/stdout using JSON-RPC 2.0. The Python server forwards parameter-change
commands to More-Phi through a cross-platform IPC bridge:

- **Windows**: named pipe `\\.\pipe\more_phi_vst3_mcp_<instanceId>`
- **macOS/Linux**: Unix domain socket at `/tmp/more_phi_vst3_mcp_<instanceId>.sock`

The C++ side (`src/AI/VST3IPCBridge`) receives commands on a dedicated thread,
dispatches them to the JUCE message thread, and executes them through the hosted
plugin's `IEditController` or More-Phi's `ParameterBridge`/`LockFreeQueue`.

## Setup

```bash
cd scripts/vst3-mcp-server
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt   # Windows
# source .venv/bin/activate && pip install -r requirements.txt  # macOS/Linux
```

## Running the server

### Manual (for testing)

```bash
# From the repository root
python scripts/vst3-mcp-server/server.py
```

The server reads JSON-RPC requests from stdin and writes responses to stdout.

### As an MCP host configuration (Claude Desktop example)

Add to your MCP host config:

```json
{
  "mcpServers": {
    "more-phi": {
      "command": "python",
      "args": [
        "G:/More_Phi-vst3-plugin/scripts/vst3-mcp-server/server.py"
      ],
      "env": {
        "MORE_PHI_INSTANCE_ID": "<instance-id-from-more-phi>"
      }
    }
  }
}
```

The `MORE_PHI_INSTANCE_ID` environment variable must match the instance id shown
in More-Phi's MCP status panel (also used for the embedded TCP MCP server port).
If omitted, the server uses `default` and expects a running More-Phi instance
that exported its registry to the default temp path.

## Tool reference

### Tier 1 — Hot path

| Tool | Description |
|------|-------------|
| `set_eq_band` | Set EQ band freq/gain/Q |
| `set_output_gain` | Set output gain (dB) |
| `set_compressor` | Set compressor ratio/threshold/attack/release |
| `set_limiter_ceiling` | Set true-peak limiter ceiling (dBTP) |
| `set_lufs_target` | Set integrated loudness target (LUFS) |

### Tier 2 — Structural

| Tool | Description |
|------|-------------|
| `set_saturation` | Set saturation drive amount |
| `set_stereo_width` | Set mid/side stereo width |
| `load_preset` | Load a named preset |
| `save_preset` | Capture current state as Base64 |

### Tier 3 — Query / state

| Tool | Description |
|------|-------------|
| `get_plugin_state` | Full state snapshot |
| `list_parameters` | Enumerate parameters |
| `reset_to_default` | Reset all parameters to defaults |
| `get_spectrum_snapshot` | Spectrum analysis snapshot (placeholder) |
| `get_lufs_reading` | Current LUFS reading (placeholder) |

### Batch

| Tool | Description |
|------|-------------|
| `apply_mastering_chain` | Apply EQ bands + compressor + limiter + output gain atomically |

## AI prompt contract

When using this server, the AI assistant should:

1. Parse the user's request into specific parameter changes.
2. Use `apply_mastering_chain` for multi-parameter requests.
3. Verify `structuredContent.status` (or the JSON `status` field).
4. Confirm exact values applied, including before/after deltas.
5. On failure, surface `error_message` and `corrective_action`.
6. Never claim a change was applied without checking the result.

Example confirmation:

```
✓ Applied: Output gain changed from +0.0 dB → -3.0 dB
✓ Applied: Limiter ceiling set to -1.0 dBTP (was -0.5 dBTP)
Execution time: 12 ms
```

## Parameter registry

At plugin load, More-Phi exports the hosted plugin's parameter descriptors to:

```
<temp>/more_phi_vst3_mcp_<instanceId>_registry.json
```

The Python server reads this file on startup and falls back to a built-in
mastering-parameter map (`scripts/vst3-mcp-server/schema/param_registry.json`)
if the exported registry is not present.

## Verification and audit

Every tool call returns a MCP `CallToolResult` that carries:

- `structuredContent` — the tool's result object, validated against the tool's
  declared `outputSchema` (self-validated server-side, since returning a
  `CallToolResult` bypasses the SDK's own output check).
- `isError` — `true` when the result `status` is not `"success"` (covers handler
  failures, IPC errors, and `partial_failure`), `false` on success. Input that
  fails `inputSchema` validation is rejected by the SDK before the handler runs.
- `content` — a `TextContent` holding the JSON-serialized result for human/legacy
  clients.

Every result object (success or failure) is stamped with uniform verification
metadata so the AI client can audit and correlate calls:

- `request_id` (`req_<8hex>`)
- `timestamp` (ISO-8601 UTC)
- `latency_ms`

Single-parameter tools (`set_eq_band`, `set_output_gain`, `set_limiter_ceiling`,
`set_lufs_target`, `set_saturation`, `set_stereo_width`) report `value_before` /
`value_after` in **physical units** (dB, Hz, %, LUFS), denormalized from the
bridge's verified readback — never the requested value. `set_eq_band` applies the
gain parameter via a dedicated `SET_PARAM` so its before/after can be verified,
with freq+Q in a follow-up batch.

`apply_mastering_chain` reports honest accounting: `requested_params`,
`applied_params`, `failed_params`, `skipped_params` (parameters that could not be
resolved in the current registry), `skipped` (names), and `failures`. Status is
`success` / `partial_failure` / `failure`.

`reset_to_default` normalizes each parameter's default against its own range
before sending (the IPC batch expects normalized `[0,1]` values).

The read-only result cache invalidates on **write** tools only (classified by the
tools' `readOnlyHint` annotation, not cache strategy), so `no_cache` reads like
`get_plugin_state` no longer wipe the `list_parameters` TTL cache.

All calls are appended to:

```
~/.mcp_vst3/audit.jsonl
```

## Testing

```bash
cd scripts/vst3-mcp-server
.venv\Scripts\python -m pytest tests/ -v
```

## C++ integration

The C++ IPC bridge is implemented in:

- `src/AI/VST3IPCBridge.h`
- `src/AI/VST3IPCBridge.cpp`

It is instantiated in `MorePhiProcessor` and started/stopped alongside the
embedded TCP MCP server. The bridge is tested in:

- `tests/Unit/TestVST3IPCBridge.cpp`

## Limitations

- `get_spectrum_snapshot` and `get_lufs_reading` are currently placeholders.
  Real-time spectrum/LUFS data is available through the embedded TCP MCP server
  (`analysis.get_summary`, `analysis.get_spectrum`).
- Preset names for `load_preset` must refer to presets the C++ host knows how
  to resolve; the bridge currently forwards the name as bytes.
