# Headless Ozone Assistant Trigger — Design

**Date:** 2026-06-20
**Status:** Approved (brainstormed), pending implementation plan
**Branch:** `feature/release-validation`

## Problem

More-Phi has three Ozone Assistant integration paths today; none of them can
trigger Ozone's real ML Assistant analysis **headlessly** (no human at the GUI):

1. **Hosted-parameter toggle** (`HostedOzonePluginBackend::runMasterAssistant`)
   sets a parameter named "assistant"/"analyze" to 1.0. Ozone 11 Pro exposes no
   such automatable parameter, so this never starts a real analysis.
2. **IPC ring injection** (`IZotopeIPCAssistant`) would publish an
   `AssistantRequest` into a shared-memory `IZOT` ring.
   `docs/OZONE_PRIVATE_IPC_LIVE_FINDINGS_20260516.md` confirms no such named
   ring exists in live FL Studio sessions. Dead end.
3. **Production capture/diff** (`tools/ozone_assistant_diff.py`) — the only
   verified-working path today, but it requires a human to click the Assistant
   button in the Ozone GUI.

The repo's own live findings state verbatim: *"the actual trigger function that
starts an Assistant analysis was never identified."* A static recon tool
(`tools/ozone_static_recon.py`) was written to find it — by locating the
phase-state strings and walking the `.pdata` call graph — but
`tools/live_captures/static/` is empty. It has never been run. That is the
"unattempted" piece.

## Goal

A workflow that **locates the Ozone Assistant trigger function statically,
certifies it against a live session read-only, and invokes it headlessly** —
producing Ozone's real ML mastering decisions as an auditable, replayable
parameter diff. Isolated (no-DAW) by default; live-DAW invocation as an
explicit opt-in behind the full safety gate.

## Scope boundary (what this is and is not)

**In scope:** driving the legitimate Ozone Assistant feature the operator owns,
on their machine, for their own audio, using the same internal code path the GUI
button uses. Interoperability research within the envelope defined by
`docs/OZONE_IPC_RESEARCH_METHODOLOGY.md`.

**Out of scope (unchanged from existing repo policy):** licensing / PACE / iLok,
anti-tamper / integrity-check logic, credentials, account / entitlement traffic.
These are never decoded, probed, or bypassed.

---

## Section 1 — Architecture

### One MCP tool, two backends

A single new tool `ozone_trigger_assistant_headless` is registered alongside the
existing `ozone_run_assistant` in `StandaloneMcpServer.cpp`. It is the only
entry point clients and the orchestration workflow call. It dispatches to one of
two backends based on `target_mode`:

```
tools/call ozone_trigger_assistant_headless  { target_mode: isolated | live }
        │
        ▼
┌─────────────────────────────────────────────────────┐
│ HeadlessAssistantTrigger (new, src/AI/StandaloneMcp) │
│  • loads trigger manifest (offset + signature)       │
│  • validates DLL hash matches manifest               │
│  • dispatches to target backend                      │
└───────────────┬─────────────────────────┬───────────┘
                │ isolated (default)       │ live (opt-in)
                ▼                          ▼
   IsolatedOzoneTrigger        LiveFridaTrigger
   (More-Phi-hosted instance,  (attaches to FL64 PID,
    in-process fn pointer)      out-of-process call)
                │                          │
                └──────────┬───────────────┘
                           ▼
            before/after hosted-parameter diff
            (reuses ozone_assistant_diff capture)
```

- **`isolated` (default):** More-Phi hosts its own Ozone Pro instance standalone
  (no DAW). The trigger function is called **in-process** — same address space,
  no attach, no cross-process injection. Reproducible and CI-able. The repo
  already hosts Ozone via `PluginHostManager`; we add one method that resolves
  and calls the discovered trigger function pointer directly.

- **`live` (opt-in):** Out-of-process. A Python launcher uses Frida to call the
  trigger inside a live `FL64.exe` PID (read-only attach + one function call).
  Carries real risk to the live session; behind the full gate (Section 3).

### The trigger manifest is the safety contract

No offset is hard-coded anywhere in the tool. Recon produces a versioned
**trigger manifest** JSON:

```json
{
  "schema_version": 1,
  "dll_sha256": "ab12…",
  "dll_path": "C:\\Program Files\\Common Files\\VST3\\iZotope\\iZOzonePro.dll",
  "trigger_rva": "0x……",
  "calling_convention": "microsoft_x64",
  "argument_layout": [
    {"index": 0, "register": "rcx", "type": "pointer", "role": "assistant_state"}
  ],
  "phase_state_markers": [
    "PROCESSING_LISTENING",
    "LEARNING_EQ_AND_CLASSIFYING_GENRE",
    "PROCESSING_SETTING_SIGNAL_CHAIN"
  ],
  "certified_at_unix": 1718800000,
  "certified_by": "operator-handle",
  "isolation_test_passed": true
}
```

Every invocation recomputes `sha256(loaded iZOzonePro.dll)` and compares to
`dll_sha256`. Mismatch ⇒ refuse. This is what makes the tool safe across Ozone
updates: a reinstall silently invalidates the manifest and the tool no-ops
rather than calling a stale offset.

> Note: the field values above (`"ab12…"`, `"0x……"`) are illustrative
> placeholders. Recon (Stage 0) fills the real SHA-256; validation (Stage 1)
> fills the real RVA. A manifest with a placeholder value is treated as
> unsigned and refused by all invocation stages.

---

## Section 2 — The five stages

Each stage produces a reviewable artifact. The orchestration workflow refuses to
advance until the previous stage's artifact is signed off (tracked in a local
state file). This mirrors the methodology doc's rule: *live mutation is the last
step, not part of discovery.*

```
Stage 0 — Recon            Stage 1 — Validate       Stage 2 — Dry-run (isolated)
 (no attach)                (read-only attach)       (in-process call, no DAW)
      │                          │                          │
      ▼                          ▼                          ▼
 trigger_candidates.json   validated_trigger.json   isolated_dry_run.json
      │                          │                          │
      └──── gate: review ────────┴──── gate: fake-target ────┘
                                                             │
                                                             ▼
                                    Stage 3 — Live (opt-in)  Stage 4 — Capture
                                     (Frida into FL64)        (param diff)
```

### Stage 0 — Static recon (no process attach; fully safe)

Run the **existing, currently-unrun** `tools/ozone_static_recon.py` against the
present `iZOzonePro.dll`. Already written and authorized (methodology §5.2:
plain-string xrefs + `.pdata` call graph).

**Output:** `tools/live_captures/static/ozone_recon.json` — function RVAs that
reference the phase strings, ranked by phase-string hit count and caller/callee
connectivity.

**Gate:** operator reviews candidates. Nothing has touched any process.

### Stage 1 — Live observer validation (read-only attach)

Recon yields candidate trigger functions, but does not prove which one *starts*
an analysis. This stage answers that by **observing, not calling**. Reuse the
repo's existing read-only tracer (`tools/izotope_state_diff_trace.py`) on each
candidate while a human starts Assistant once in the GUI (the only manual step
in the whole workflow).

The candidate whose state goes idle → `PROCESSING_LISTENING` →
`…SETTING_SIGNAL_CHAIN` **immediately** after the click, with `arg2` payload
growth, is the real trigger. This is the failure mode the live-findings doc
explicitly warns about with `+0xEAD3E0` (a poller, not a trigger).

**Output:** `validated_trigger.json` — exactly one function RVA + calling-
convention signature + `dll_sha256`.

**Gate:** trace must show a clean state transition causally tied to the click,
not idle polling.

### Stage 2 — Isolated dry-run (in-process call, no DAW)

The certified trigger is called against a **More-Phi-hosted standalone Ozone
instance** — no FL Studio. `HeadlessAssistantTrigger` resolves the function
pointer inside its own address space (DLL already loaded by the host), feeds a
known audio file, calls the trigger, and confirms a real analysis ran by watching
the same phase-state transition **plus** a non-empty parameter diff.

**Output:** `isolated_dry_run.json`.

**Gate:** must be a fake-target test first (we call the trigger against an Ozone
instance whose parameters we control, prove it produces a sane diff, and only
then trust the offset). This is methodology §4's "reproduce the layout in a fake
segment / harness first," applied to a function call. Precedent:
`IZotopeIPCAssistant::setFakeSegmentForTests`.

### Stage 3 — Live invocation (opt-in, full gate)

Only reached if `target_mode=live`. Out-of-process Frida script
(`tools/ozone_trigger_frida.js`) calls the trigger inside a live `FL64.exe` PID.

Behind the repo's two-part gate: env `MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1` **and**
per-call `allow_unsafe_write=true`. Defaults to **capture-only**
(`apply_result=false`) — the Assistant computes its recommendations but they are
**not** written to the project until explicitly replayed.

**Gate:** Stage 2 must have passed **and** DLL hash matches; otherwise no-op.

### Stage 4 — Capture (reuses existing tooling)

Before/after hosted-parameter diff via the existing `tools/ozone_assistant_diff.py
capture` / `diff`, producing a replayable `assistant_diff.json`. This is the
**output** of the whole workflow — Ozone's real ML mastering decisions as an
auditable, replayable parameter set, in the same format the production workflow
already uses. No new artifact format.

### Stage notes

1. Stage 1 is the only step needing a human (one GUI click for correlation).
   Stages 0, 2, 3, 4 are fully automated.
2. The workflow **ends** at Stage 4 by default with capture-only output. Whether
   to apply (`Stage 3 apply_result=true` or the replay tool) is always a
   separate, deliberate decision.

---

## Section 3 — Safety envelope

Four layers, each must pass or the call no-ops. This extends the repo's existing
two-part write gate for function-call invocation.

### Layer 1 — DLL identity pinning (the hard safety contract)

The trigger manifest stores `sha256(iZOzonePro.dll)` at recon time. **Every**
invocation — isolated or live — recomputes the hash of the loaded DLL and
compares. Mismatch ⇒ `dll_hash_mismatch`, never call. An Ozone update that
shifts the trigger offset cannot silently turn a stale offset into a corrupted
call. The hash also pins the manifest to one Ozone build, so the workflow is
reproducible.

### Layer 2 — Authorization scope (the compliance contract)

This is not DRM circumvention: we drive the legitimate Assistant feature the
operator owns, on their machine, for their own audio, using the same internal
code path the GUI button uses. To make that posture explicit and auditable,
every run emits a structured **authorization record** (methodology §1 already
mandates this): product/version, host/version, OS build, PID, operator, scope
statement, timestamp.

The workflow refuses to run Stage 3 (live) unless an authorization scope file
named `tools/live_captures/ozone_trigger_authorization.json` is present,
containing the methodology-§1 fields (product/version, host/version, OS build,
PID, operator, scope statement, timestamp). It does **not** touch:
licensing/PACE/iLok, anti-tamper/integrity checks, credentials, or account
traffic — out of scope, as the live-findings doc states.

### Layer 3 — The two-part invocation gate

Live invocation (Stage 3) requires **both**:

- Env gate: `MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1`
- Per-call gate: `allow_unsafe_write=true`

Isolated invocation (Stage 2) needs neither — it is in-process on a
More-Phi-hosted instance, so there is no production session to protect. This is
what makes "isolated default, live opt-in" a real safety property, not just a
CLI flag.

### Layer 4 — Capture-only by default; apply is separate and transactional

Stages 3/4 default to `apply_result=false`: the Assistant runs, its
recommendations are captured as a before/after diff, but **nothing is written to
the project**. Applying is a separate call (`ozone_assistant_diff.py apply` or
`set_parameters_batch`), reusing the repo's existing transactional apply —
"either every parameter is valid and applied, or none are" (capabilities doc,
Process Optimization).

### Failure-mode handling

| Condition | Behavior |
|---|---|
| DLL hash ≠ manifest | `dll_hash_mismatch` — refuse, never call |
| Manifest missing/unsigned | `trigger_not_certified` — Stages 2–4 refuse |
| Stage 2 dry-run never passed | `isolated_certification_missing` — Stage 3 refuses |
| Frida attach fails / FL64 not found | `live_attach_failed` — no-op, no partial state |
| Trigger called but no phase transition observed | `trigger_did_not_fire` — flagged, parameters not applied |
| Anti-tamper heuristic tripped | Out of scope — not probed; presents as Ozone crash/log |

### Residual risk (stated honestly)

Calling an internal vendor function can trip Ozone's own integrity/anti-tamper
heuristics, or crash the process if the offset is subtly wrong. Hash-pinning +
isolated-certification-first ordering makes this **unlikely**, but it cannot be
guaranteed impossible. That residual risk is why Stages 0–2 exist on a non-live
target and why live is opt-in.

---

## Section 4 — File layout & testing

### New files

```
src/AI/StandaloneMcp/
  HeadlessAssistantTrigger.h          // interface + isolated/live backends
  HeadlessAssistantTrigger.cpp        // manifest load, hash pin, dispatch
  TriggerManifest.h                   // struct: dll_sha256, trigger_rva,
                                      //   calling_convention, phase_states
docs/
  ozone_trigger_manifest.json         // the versioned contract artifact
  superpowers/specs/
    2026-06-20-headless-ozone-assistant-design.md   // this document
tools/
  ozone_trigger_workflow.py           // orchestration driver (stages 0→4)
  ozone_trigger_frida.js              // live Stage-3 call script
  ozone_trigger_correlate.py          // Stage-1: match click → phase transition
tests/Unit/
  TestHeadlessAssistantTrigger.cpp    // unit + gate tests
```

### Modified files (minimal, follow existing patterns)

- `src/AI/StandaloneMcp/StandaloneMcpServer.cpp` — register one new tool
  `ozone_trigger_assistant_headless`, following the exact `ozone_run_assistant`
  registration block (lines 300–328). Handler dispatches on `target_mode`.
- `tests/CMakeLists.txt` — add `Unit/TestHeadlessAssistantTrigger.cpp`
  (line ~194, next to the IPC assistant test).

### No changes to

`OzonePluginBackend`, `IZotopeIPCAssistant`, the audio engine, or anything on
the audio thread. The new tool is purely additive and lives entirely in the MCP
layer.

### Testing strategy — three tiers (mirrors IPC assistant test precedent)

| Tier | What | How | Gates which stage |
|---|---|---|---|
| **Unit (fake-target)** | Manifest loading, hash pin, gate logic, dispatch routing | `setFakeTriggerForTests()` — inject a fake function pointer (the `setFakeSegmentForTests` pattern). Tests: hash mismatch ⇒ refuse; env-gate off ⇒ refuse; per-call flag off ⇒ refuse; valid manifest + isolated ⇒ calls fake trigger, returns success. | Always runs in CI |
| **Isolated integration** | End-to-end vs. a real More-Phi-hosted Ozone instance, no DAW | `TestHeadlessAssistantTrigger.cpp` guarded by `MORE_PHI_HAVE_OZONE` (compiles/runs only when a real Ozone DLL is present). Drives a known audio file, calls trigger, asserts non-empty param diff. | Unlocks Stage 3 (live) |
| **Manual runbook** | Stages 1 & 3 against live FL Studio | Not automated (needs a human click for correlation; needs a live DAW). Documented as a runbook with exact commands. | Operator-run |

### Reproducibility note

The trigger manifest's `dll_sha256` pins the Ozone build, so a certified result
is reproducible **for that exact DLL**. Across an Ozone update, the manifest
must be re-derived via Stages 0–2 — by design, not by accident.

---

## Open questions for the implementation plan

These are deferred to the `writing-plans` stage, not blockers for this design:

1. Exact calling-convention probing method for Stage 1 (how many args, which
   registers) — needs the recon output first.
2. Whether the isolated Stage-2 backend reuses the headless-render harness's
   audio-feeding path or the `OzonePluginBackend::renderInputAudio` path.
3. Frida vs. Detours-vs.-native-loader trade-off for the live Stage-3 call —
   Frida is the existing precedent (`tools/trace_izotope_ipc_frida.js`).

## Related artifacts

- `docs/OZONE_IPC_RESEARCH_METHODOLOGY.md` — the safety envelope this extends
- `docs/OZONE_PRIVATE_IPC_LIVE_FINDINGS_20260516.md` — prior read-only findings
- `docs/OZONE_IPC_ASSISTANT_CAPABILITIES.md` — existing Assistant capability
- `tools/ozone_static_recon.py` — the unrun recon tool (Stage 0)
- `tools/ozone_assistant_diff.py` — the capture/diff tool reused (Stage 4)
- `tools/izotope_state_diff_trace.py` — the read-only tracer reused (Stage 1)
- `src/AI/StandaloneMcp/IZotopeIPCAssistant.*` — fake-target test precedent
