# Research Quarantine

This directory contains **experimental research artifacts** that were previously mixed with production code. These files are preserved for archaeology, reference, and potential future revival, but they are **not part of the active build or release pipeline**.

## Quarantine Date
2025-06-21

## Rationale
These artifacts were created during deep R&D into reverse-engineering the Ozone 11 mastering plugin's IPC protocol, UI automation triggers, and headless assistant capabilities. They served their purpose in understanding the integration surface, but they create noise in the production codebase, increase clone size, and confuse new contributors about what is actually shipping.

## Directory Structure

```
research/
  ozone-probes/          # Ozone UI automation, disassembly, and trigger scripts
  izotope-research/      # iZotope IPC trace, decode, and state-diff tools
  live-captures/         # Screenshots, logs, and JSON evidence from live probing
  temp-verification/     # Temporary verification artifacts (.tmp_verify)
  ipc-captures/          # IPC transport inventory and capture notes
  pluginval_report.txt   # One-off pluginval validation report
  OZONE_HEADLESS_ASSISTANT_RUNBOOK.md
  OZONE_HEADLESS_TRIGGER_FINDINGS.md
  OZONE_HEADLESS_TRIGGER_FINDINGS_20260620.md
  OZONE_IPC_ASSISTANT_CAPABILITIES.md
  OZONE_IPC_RESEARCH_METHODOLOGY.md
  OZONE_PRIVATE_IPC_LIVE_FINDINGS_20260516.md
  MCP_OZONE_IMPLEMENTATION_GUIDE.md
  ozone_ipc_default_flat_manifest.json
  ozone_ipc_global_session_manifest.json
```

## What Was Moved

### 1. Ozone Probe Scripts (`ozone-probes/`)
- **Count**: ~50 files (Python + JavaScript)
- **Purpose**: Reverse-engineering Ozone 11's internal UI automation, button state probing, disassembly polling, trigger detection, vtable analysis, and wizard actionables.
- **Key files**:
  - `ozone_disasm_*.py` / `ozone_disasm_*.js` — Disassembly and static analysis
  - `ozone_gated_trigger.js` — Gated trigger detection
  - `ozone_headless_assistant.js` — Headless assistant automation
  - `ozone_invoke_trigger.js` / `ozone_live_context.js` — Trigger and context probing
  - `ozone_probe_controller.js` / `ozone_rtti_probe.js` — Controller and RTTI probing
  - `run_ozone_*.py` / `run_host_*.py` — Runner scripts for various probe scenarios
  - `probe_ozone_params.py` / `decode_ozone_assistant_arg2.py` — Parameter decoding

### 2. iZotope Research (`izotope-research/`)
- **Count**: ~10 files
- **Purpose**: iZotope IPC protocol tracing, state diff analysis, and cross-reference mapping.
- **Key files**:
  - `izotope_ipc_decode.py` / `izotope_ipc_xref.py` — IPC decode and cross-reference
  - `izotope_state_diff_trace.py` — State diff tracing
  - `trace_izotope_ipc_frida.js` — Frida-based IPC tracing
  - `run_izotope_ipc_trace.ps1` — PowerShell runner
  - `relay_link_interceptor.py` — Link interception

### 3. Live Captures (`live-captures/`)
- **Count**: ~50 files (screenshots, logs, JSON evidence)
- **Purpose**: Evidence collected during live Ozone UI automation sessions.
- **Contents**:
  - `shots/` — BMP/PNG screenshots of Ozone UI states during automation
  - `static/` — Static probe scripts, JSON recon data, test tones, and analysis reports
  - `arm_body_log.txt`, `arm_log.txt`, `run_log.txt` — Execution logs
  - `full_trigger_evidence.json`, `gated_trigger_result.json`, `invoke_trigger_evidence.json` — JSON evidence

### 4. Temporary Verification (`temp-verification/`)
- **Count**: ~15 files
- **Purpose**: Ad-hoc verification scripts, HTML exports, and CSV data from various research threads.
- **Contents**: Python scripts, JSON files, CSV exports, and HTML captures from web research.

### 5. IPC Captures (`ipc-captures/`)
- **Count**: 1 file
- **Contents**: `ipc_transport_inventory_20260516.md` — IPC transport inventory from May 2025

### 6. Ozone Research Documentation
- **Count**: 8 files
- **Purpose**: Methodology docs, capability assessments, trigger findings, and implementation guides for the Ozone headless assistant integration.
- **Files**:
  - `OZONE_HEADLESS_ASSISTANT_RUNBOOK.md`
  - `OZONE_HEADLESS_TRIGGER_FINDINGS.md`
  - `OZONE_HEADLESS_TRIGGER_FINDINGS_20260620.md`
  - `OZONE_IPC_ASSISTANT_CAPABILITIES.md`
  - `OZONE_IPC_RESEARCH_METHODOLOGY.md`
  - `OZONE_PRIVATE_IPC_LIVE_FINDINGS_20260516.md`
  - `MCP_OZONE_IMPLEMENTATION_GUIDE.md`
  - `ozone_ipc_default_flat_manifest.json` / `ozone_ipc_global_session_manifest.json`

## What Was NOT Moved (Production)

The following directories and files remain in their original locations as they are **active production components**:

- `tools/export_onnx/` — ONNX model export pipeline for neural mastering
- `tools/headless_mastering_render/` — Headless mastering render harness (CMake target)
- `tools/inference_server/` — Python inference server for real-time neural mastering
- `tools/pluginval.exe` / `tools/pluginval.zip` — Plugin validation tool
- `tools/release-validate.bat` — Release validation script
- `scripts/neural-mastering/` — Training pipeline and data generation for neural mastering
- `docs/ECOSYSTEM.md` / `docs/ARCHITECTURE.md` / etc. — Production documentation
- `store-backend/` — Production licensing/Stripe backend service
- `landing-page/` — Product website

## Build Impact

**None.** These files were never referenced in `CMakeLists.txt`, `package.json`, or any production build script. Moving them to this directory does not affect compilation, linking, or test execution.

## Revival Path

If any of this research becomes relevant again (e.g., a new Ozone integration approach):

1. Review the `OZONE_IPC_RESEARCH_METHODOLOGY.md` for context
2. Examine `ozone-probes/` for the specific scripts that were effective
3. Check `live-captures/shots/` for visual evidence of what worked
4. Move the needed files back into `tools/` or `src/` as appropriate

## Note to Contributors

Do not add new experimental scripts to `tools/` or `docs/` without first deciding whether they belong here. If a script is for one-time research, temporary probing, or reverse-engineering a third-party tool, **quarantine it immediately** after use.
