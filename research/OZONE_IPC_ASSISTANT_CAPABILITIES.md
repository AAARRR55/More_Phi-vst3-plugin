# Ozone IPC Assistant Capability Overview

## Overview

When a manifest-defined iZotope Assistant ring is available, `ozone_run_assistant` provides a controlled bridge between More-Phi's MCP control plane, the hosted Ozone plugin, and iZotope's IPC Assistant protocol. The assistant can request Ozone-side analysis, receive normalized parameter decisions, optionally apply those decisions to the hosted plugin, and expose supporting IPC inspection tools for diagnostics and reverse engineering.

For the authorized research workflow used to identify transports, capture
read-only evidence, and reconstruct candidate message schemas, see
[iZotope IPC Research Methodology](OZONE_IPC_RESEARCH_METHODOLOGY.md).

Live Ozone 11 testing on Windows confirmed a different production reality: Ozone Pro and Visual Mixer expose named iZotope mutexes, but no named or signature-identifiable `IZOT` shared-memory ring was found in FL Studio. More-Phi therefore leaves live IPC injection blocked unless a real segment is explicitly discovered and verified. The production-ready Assistant workflow is the normal hosted Ozone Assistant UI path plus MCP parameter capture and diffing.

This is best understood as an operational layer around Ozone Assistant, not a general chatbot. Its value is direct, structured automation of mastering-analysis results into inspectable and optionally actionable parameter changes.

Current verified production path:

1. More-Phi forwards DAW audio and playhead context into hosted Ozone.
2. The operator runs Ozone Assistant normally in the Ozone UI.
3. More-Phi captures hosted Ozone parameters before and after the Assistant run.
4. The diff becomes the auditable and replayable Assistant result.

## Real-Time Data Analysis

The IPC assistant implementation can inject an `AssistantRequest` into a manifest-defined IPC ring, wait for `AssistantResult`, and parse parameter decisions into structured JSON when such a ring is present. In the currently tested Ozone Pro build, the ring was not exposed as a named shared-memory mapping.

It can answer operational queries such as:

- What parameter decisions did Ozone Assistant produce for this instance?
- Which Ozone instance responded to this Assistant request?
- Did the returned result contain valid normalized values?
- Was the result fresh, or was it stale data from an earlier ring state?

Value-add:

- Converts opaque shared-memory IPC traffic into typed, actionable data.
- Request watermark guarantees result freshness. The ring write index at the moment the `AssistantRequest` is published is recorded, and only `AssistantResult` frames whose ring position is at or after that watermark are accepted. This creates a hard ordering guarantee that ties the result causally to the current invocation.
- Stale result suppression. `AssistantResult` frames predating the current request are silently ignored by the watermark filter. This is not reported as an error; polling continues until a fresh matching result arrives or the timeout expires.
- Recovers from corrupt bytes and ignores unrelated IPC frames.
- Returns normalized parameter decisions suitable for automation or audit.
- In live Ozone 11 sessions where the IPC ring is not discoverable, More-Phi captures the Assistant effect by diffing hosted-plugin parameter snapshots before and after a normal Assistant UI run.

## Automated Reporting

The IPC tooling can produce snapshots, dumps, and capture logs around assistant activity.

Available actions include:

- `izotope_ipc_status`: report current IPC attachment state.
- `izotope_ipc_snapshot`: read bounded byte ranges and report candidate `IZOT` frames.
- `izotope_ipc_dump`: write raw IPC byte windows to local files.
- `izotope_ipc_capture`: record bounded diff samples over time, optionally as JSONL.
- `ozone_run_assistant`: return structured `AssistantResult` metadata and parameters.
- `tools/ozone_assistant_diff.py capture`: capture the current hosted Ozone parameter state through MCP.
- `tools/ozone_assistant_diff.py diff`: compare pre- and post-Assistant snapshots and emit replayable parameter changes.
- `tools/ozone_assistant_diff.py apply`: replay a captured diff through More-Phi's parameter queue.

It can resolve reporting questions such as:

- What changed in the IPC segment during Assistant execution?
- Which frame candidates appeared after triggering analysis?
- What raw bytes back the `AssistantResult`?
- What parameters were recommended, and were they applied?
- What Ozone parameters changed after a manual Assistant run?

Value-add:

- Creates reproducible artifacts for engineering review.
- Supports before/after IPC diffing without a debugger.
- Gives operators a machine-readable audit trail of assistant decisions, either from IPC `AssistantResult` frames or from hosted-parameter before/after diffs.

## Technical Troubleshooting

The assistant layer includes hardened diagnostics for protocol, memory, and ring-buffer issues.

Specific troubleshooting capabilities:

- Detect missing or invalid schema manifests.
- Detect missing shared-memory segment names.
- Detect Ozone instance lookup failures.
- Detect full IPC rings before writing.
- Detect oversized `AssistantResult` payload claims.
- Detect incomplete frames and timeout cleanly.
- Validate write gates before any mutation.
- Silently skip stale pre-request `AssistantResult` frames via watermark filter.
- `ipc_magic_probe`: when `MORE_PHI_DEBUG_IZOTOPE_IPC_MAGIC=1`, includes segment-level byte inspection in the tool response and emits the same data to the JUCE log. It reports expected magic, read/write indices, request start index, request watermark, and actual bytes at ring-zero, read-index, request-frame, and next-candidate positions. This is diagnostic metadata, not a failure condition, and does not prevent the normal poll loop from running.
- Live transport mismatch detection: if named mapping probes and read-only section scans find no `IZOT`, `Ozone`, `iZotope`, `Assistant`, or `IPC` signatures, `ozone_run_assistant` remains blocked and the parameter-diff workflow is used instead.

Value-add:

- Reduces live-session debugging to typed errors plus optional byte-level probes.
- Makes IPC compatibility checks repeatable across Ozone versions.
- Avoids silent corruption by refusing unsafe writes.

## Process Optimization

The assistant layer can turn Ozone Assistant recommendations into a controlled automation workflow through either verified IPC results or hosted-parameter diffs.

Implemented actions:

- Run Assistant in capture-only mode by default.
- Optionally apply returned normalized parameter decisions with `apply_result=true`.
- Validate all parameter indices and normalized values before applying. If any decision is invalid, the assistant rejects the entire apply operation and performs no parameter mutation. This provides transactional behavior for `apply_result=true`: either every requested parameter write is valid and applied, or none are applied.
- Report applied count, requested count, and per-parameter application details.
- Capture pre- and post-Assistant hosted Ozone parameter snapshots with `tools/ozone_assistant_diff.py`.
- Produce replayable diffs that can be reviewed, stored, or applied later through `set_parameters_batch`.

Queries it can resolve:

- What would Ozone Assistant change if I ran it now?
- Can these recommendations be safely applied to the hosted plugin?
- Which parameters were applied successfully?
- Did any Assistant result contain unsafe or invalid values?

Value-add:

- Separates recommendation capture from destructive application.
- Enables review-first mastering workflows.
- Provides deterministic, transactional failure behavior before plugin state is changed.
- Keeps the production workflow independent of unconfirmed private iZotope transport internals.

## Cross-Platform and Host Integration

The live IPC write path is Windows-first because it relies on `OpenFileMappingW` and `MapViewOfFile`. The broader More-Phi/MCP layer remains structured so clients can invoke tools consistently through JSON-RPC.

Integration capabilities:

- Discover DAW process IDs for manifest segment-name templates on Windows.
- Resolve segment names from explicit args, environment variables, or manifest templates.
- Attach to real IPC memory with RAII cleanup.
- Use fake mutable segments for deterministic CI coverage.
- Operate through MCP `tools/list` and `tools/call`.
- Coordinate with hosted-plugin tools such as parameter listing, state save/restore, and Ozone parameter application.

Live IPC write is Windows-only in this release. Read-only attach and fake-segment testing work cross-platform. POSIX read-only attach exists in discovery via `shm_open` and `mmap`, but writable live invocation currently returns `unsupported_platform` outside Windows. POSIX write support is deferred pending confirmation of iZotope's POSIX IPC segment naming convention.

Live Ozone 11 status: FL Studio sessions with Ozone Pro and Visual Mixer exposed `iZotopeOZONEPROMSMutex` and `iZotopeVISUALMIXERMutex`, but exhaustive read-only probing found no named mapping matching the mutex-derived naming convention and no accessible section containing `IZOT` or iZotope Assistant signatures. This indicates that iZotope's public inter-plugin communication path is not the manifest-defined shared-memory ring assumed by `ozone_run_assistant`.

Value-add:

- Provides a uniform automation interface over plugin hosting, IPC inspection, and Assistant invocation.
- Supports CI-safe testing without a DAW while preserving a path to live DAW integration.
- Keeps dangerous live IPC writes behind both an argument gate and an environment gate.

## Operational Workflow

An IPC operator workflow is:

1. Attach or identify IPC context.

   Use schema path, segment name, DAW process ID, or environment variables.

2. Verify protocol compatibility.

   On first use with a new Ozone version, run `izotope_ipc_snapshot` before enabling write gates to confirm visible frame candidates, magic byte order, ring pointer offsets, and registry layout against the active manifest. For the first controlled live invocation, enable `MORE_PHI_DEBUG_IZOTOPE_IPC_MAGIC=1` so `ozone_run_assistant` includes the `ipc_magic_probe` diagnostic field in its response. On subsequent runs with a validated schema, this verification step can be skipped.

3. Enable write gates.

   Set `MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1` and pass `allow_unsafe_write=true`.

4. Invoke Assistant.

   Call `ozone_run_assistant` with timeout, observer ID, and optional Ozone instance ID.

5. Review result.

   Inspect `assistant_result`, `parameters`, source instance, target instance, elapsed time, and optional `ipc_magic_probe` fields.

6. Apply if desired.

   Pass `apply_result=true` to validate and apply normalized parameter decisions to the hosted plugin. The operation is transactional: no parameters are written unless all decisions pass validation.

7. Capture artifacts if troubleshooting.

   Use diff capture or dumps for repeatable analysis.

The production Assistant capture workflow is:

1. Capture a baseline:

   `python tools/ozone_assistant_diff.py capture --port 30001 --token "<token>" --out before.json`

2. Run Ozone Assistant manually in the hosted Ozone UI while FL Studio is playing.

3. Capture the post-Assistant state:

   `python tools/ozone_assistant_diff.py capture --port 30001 --token "<token>" --out after.json`

4. Generate the replayable diff:

   `python tools/ozone_assistant_diff.py diff --before before.json --after after.json --out assistant_diff.json`

5. Replay the diff if desired:

   `python tools/ozone_assistant_diff.py apply --port 30001 --token "<token>" --diff assistant_diff.json`

## Document Variants

For distribution, split into:

- Executive capability overview: outcomes, safeguards, workflows, and integration value. Omit ring offsets, byte-order details, and diagnostic probe fields.
- Engineering appendix: manifest schema, ring protocol behavior, watermark semantics, `ipc_magic_probe` field reference, platform limitations, and CI coverage strategy.

This preserves the "operational layer around Ozone Assistant, not a general chatbot" framing in the briefing version while giving engineers the protocol-level reference they need without abstraction mismatch.

## Executive Value

The Assistant integration adds a high-leverage automation layer: Ozone's Assistant can now be treated as a structured decision source inside More-Phi's MCP ecosystem. Users gain auditable recommendations, optional automated application with transactional safety, hardened error handling, and protocol-level diagnostics. In the current live Ozone 11 build, the production workflow is parameter capture and diffing after a normal Assistant run; live shared-memory injection remains disabled until a verified ring transport is discovered.
