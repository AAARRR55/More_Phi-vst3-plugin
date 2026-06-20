# Headless Ozone Assistant Trigger — Empirical Findings (2026-06-20)

**Status: objective NOT met.** The Master Assistant analysis was never triggered
headlessly. This document records what was tried and why, so the path is fully
mapped for a future live run.

## What was verified (real)

1. **Trigger located statically.** `0xd58a20` (thiscall), reached via the MSVC
   this-adjustor vtable thunk `0xd572f0`
   (`push rbx; sub rsp,0x20; add rcx,8; mov rbx,rdx; call 0xd58a20; ret`).
   Both disassembled and confirmed independently.
2. **Vtable resolved.** Base `imagebase + 0x28a9448` (runtime
   `0x7ffd34309448` in this build), 5 entries; thunk `0xd572f0` at index 2.
   One `.rdata` slot holds the thunk pointer (corroborates prior
   `probe_finalize_out.json` `controller_chain_head` slot `0x28a9458`).
3. **Isolated hosting works.** `MorePhiMcpServer.exe` with
   `OZONE_VST3_PATH` hosts Ozone Pro fully headless: `loaded:true`, 646 params,
   44100 Hz / 512 samples, **no DAW**. The Master Assistant controller object
   *does* instantiate standalone (vptr == vtable base, confirmed).
4. **The trigger WAS invoked headlessly**, multiple times, against the live
   controller in isolated processes.

## What failed (also real)

Every call to `0xd58a20` (and `0xe9fc30`) faulted; the before/after
hosted-parameter diff was **0** in every attempt. No phase-state transition
(`PROCESSING_LISTENING` / `LEARNING_EQ_AND_CLASSIFYING_GENRE` /
`PROCESSING_SETTING_SIGNAL_CHAIN`) was ever observed.

| Call | Context | Result |
|---|---|---|
| `0xd58a20` via thunk | isolated, idle | access violation `0xa874`, 0 diff |
| `0xd58a20` via thunk | isolated, after audio feed attempt | `0xb354` / `0x180`, 0 diff |
| `0xe9fc30` pipeline root | isolated | `breakpoint triggered` (int3 guard), 0 diff |

### Candidate classification (all 6 exhausted)

From disassembly of the 5 vtable slots + pipeline root:

- `0xd57310` (vt0): **factory** — `mov ecx,0x50; call operator_new`. Not a trigger.
- `0xd1b930` (vt1): **stub** — `xor eax,eax; ret`. Returns 0.
- `0xd572e0` (vt3): **accessor** — `lea rax,[rip+...]; ret`. Returns a static ptr.
- `0xd572a0` (vt4): **destructor** — calls dtor helper `0xd1f390` on `+0x30`/`+0x10`.
- `0xd58a20` (trigger, prior conf 0.72): thiscall orchestrator; faults (deref of
  uninitialized nested sub-objects `[rdi]`, `[rax+0x20]`).
- `0xe9fc30` (pipeline root, prior conf 0.45): 4-arg, needs a pre-built context
  struct; faults (int3 guard).

4 of 6 are structurally impossible triggers; the 2 plausible ones fault.

### Why static discovery hit a wall

Three independent static methods each returned **0 code references**:

- Phase-string RIP-xrefs (`PROCESSING_LISTENING` etc.): 0 (strings live in data tables).
- Phase-string *table* RIP-xrefs (`~0x25c5af0`): 1 ref, outside any function.
- "Master Assistant" string RIP-xrefs: 0 (243 occurrences, all in data tables).

Conclusion: Ozone's Assistant state machine is **table-driven and dynamically
indexed**. The GUI Play → analysis path is dispatched via runtime-computed
indirection that disk-only analysis cannot resolve. This is consistent with the
prior reachability result (0 bridges; ~77k register-indirect call sites, 0 in
the controller cluster).

## Why `0xd58a20` faults in a hosted (no-DAW) Ozone

The body immediately dereferences nested controller sub-objects and calls
`0x180d277f390` / `0x180d2780c80`. In a hosted-but-idle Ozone those sub-objects
(audio analysis stream + target profile) are null. `0xd58a20` is a **mid-pipeline
orchestrator**, not a self-contained "start analysis" entry. Standalone hosting
creates the controller shell but not the full GUI-session wiring it requires.

## Correction to prior conclusions

- Commit `f632856` message overclaimed: it said "the Assistant trigger cannot be
  found by static analysis" and "drives the design pivot to live-observation-
  primary." Both are wrong. The trigger (`0xd58a20`) WAS found statically; the
  live-primary pivot was reversed. This file supersedes that message.
- The static **confidence ranking** (0xd58a20 @ 0.72, 0xe9fc30 @ 0.45) is
  **empirically falsified** as trigger *entries* — they are real functions in the
  pipeline, but neither is callable to start an analysis from the controller
  alone.

## Only remaining path

Invoking `0xd58a20` against the **live** controller in a running FL64 session
(where Ozone's full Assistant session wiring exists). This was NOT attempted —
it requires explicit operator authorization to accept crash/corruption risk to
the live session, and the write gate (`MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE`) was
never enabled. Honest caveat: the static evidence suggests `0xd58a20` may fault
even live (it is likely the wrong entry), so a live attempt is uncertain, not
guaranteed.

## Artifacts

- `tools/ozone_trigger_reach.py` — reachability + indirect-call resolver
- `tools/live_captures/static/isolated_trigger_probe.py` — host Ozone + resolve controller
- `tools/live_captures/static/isolated_trigger_fire.py` / `_fire2.py` — invoke trigger, capture diff
- `tools/live_captures/static/candidate_sweep.py` — sweep all 6 candidates
- `tools/live_captures/static/find_phase_table_refs.py` — phase-table xref search
- `tools/live_captures/static/probe_*.py` + reports — prior probe work (bundled, scratch)
- Live (read-only) session evidence: FL64 PID 37864, `iZOzonePro.dll` base `0x7ffd31a60000`
