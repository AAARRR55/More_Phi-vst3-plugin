# Ozone Headless Assistant Lab Runbook

This runbook describes how to invoke iZotope Ozone's internal Master Assistant
trigger **headlessly**, inside a **More-Phi-owned host process**, under an
explicit, multi-part safety gate. It is the lab counterpart to the read-only
research in `docs/OZONE_IPC_RESEARCH_METHODOLOGY.md`.

> **READ EVERY RED LINE BELOW BEFORE RUNNING ANYTHING.** The default mode of
> this harness is DRY-RUN: it resolves the trigger, renders audio, and captures
> a parameter baseline **without calling** the internal function. The live call
> only happens when a human deliberately satisfies every gate in Section 4.

---

## 1. What this harness is (and is not)

**Is:**
- A lab tool that hosts the Ozone Pro VST3 inside a More-Phi-owned process
  (`MorePhiMcpServer` via `HostedOzonePluginBackend`), attaches Frida to *that
  process only*, resolves Ozone's internal Master Assistant trigger by RVA, and
  under a gate invokes it through the controller object's vtable slot.
- Offline-derivable: the trigger RVA, signature, and vtable topology were
  produced by static analysis of the on-disk `iZOzonePro.dll` (capstone, pefile,
  string xref, `.pdata` call-graph) per methodology sec 5.2. No Frida was
  attached to any vendor/DAW process to produce it.

**Is not:**
- A way to attach to FL Studio, iZotope's own app, PACE/iLok, or any vendor
  helper. The harness **never** attaches to anything other than the
  `MorePhiMcpServer` subprocess it spawned itself.
- A verified "start analysis" action. Per the verify verdict (Section 6), the
  candidate `0xD58A20` is a genuine Master-Assistant-cluster vtable method
  distinguished from the poller, but its *precise runtime semantics* are **not**
  confirmed by dynamic correlation (which is forbidden). It may be
  parameter-metadata enumeration rather than analysis-start. The harness is a
  research probe: invoke it, then **inspect the parameter deltas** to learn what
  it actually does.

---

## 2. The trigger surface (from offline static recon)

| Item | Value | Notes |
|------|-------|-------|
| Module | `iZOzonePro.dll` | Image base `0x180000000` |
| Body RVA | `0xD58A20` | The controller method body (4848 bytes) |
| **Vtable thunk RVA** | **`0xD572F0`** | **The dispatch surface. Invoke THIS, not the body.** |
| Signature | MSVC x64 thiscall, 3 args | `int thiscall RunAnalysis(this RCX, inOutCtx RDX, optsOrOut R8)` |
| Poller RVA | `0xEAD3E0` | READ-ONLY observation anchor. NOT the trigger. Never call. |
| Applier RVA | `0xEAD930` | **FORBIDDEN.** The APPLY path. Never call, never instrument. |

The thunk at `0xD572F0` is a 27-byte MSVC multiple-inheritance this-adjustor:

```
push rbx
sub  rsp, 0x20
add  rcx, 8          ; this-adjust to the base sub-object
mov  rbx, rdx
call 0xD58A20        ; tail-call the body
mov  rax, rbx
ret
```

This is the canonical GUI-Play dispatch shape. A harness **must** invoke through
this thunk (the vtable slot), never the raw body `0xD58A20`, because:

1. The body expects `rcx` already adjusted to the `+8` base sub-object.
2. The vtable slot is the address the GUI Play button actually reaches; calling
   the raw body bypasses the adjustor and faults on `mov rbx,[rcx]`.

---

## 3. Preconditions for a *correct* call (crash avoidance)

The verify verdict confirmed the body dereferences several controller fields
before doing anything substantive. **All four pointer arguments must be live,
fully-constructed objects sourced from the hosted instance** — never
synthesized. A fake `this` faults on the very first `mov rbx,[rcx]` then again
on `cmp byte [rbx+0x90]`.

1. **A live hosted Ozone instance** must exist inside the `MorePhiMcpServer`
   process, fully initialized through the normal VST3 host API
   (`HostedOzonePluginBackend` loads it via `OZONE_VST3_PATH`). The controller
   object must be fully constructed.
2. **RCX** must point at the controller object's base sub-object. The driver
   passes the controller base; the thunk applies the `+8` adjust internally.
3. **RDX** must be a valid in/out context (preserved across the whole body).
4. **R8** must point at a writable output/options struct (consumed via
   `mov rdi,[r13]`).
5. **A valid/armed audio signal chain** must be present. The driver renders the
   `--audio` file through Ozone before invoking, which arms the downstream
   pipeline (`0xE9FC30 -> ... -> 0xEABDB0`) that reads audio and drives the
   poller. Without audio the analysis stages but never advances.

The `ozone_headless_assistant.js` `validateControllerThis()` guard checks that
`[this]` (the vtable pointer) lands inside `iZOzonePro.dll`'s mapped range and
that `[this+0x90]`, `[+0x98]`, `[+0xA0]`, `[+0xA8]` are readable before any
dispatch — so a partially-constructed object fails loudly in the guard rather
than inside the call.

> **How to obtain a live `this`/RDX/R8:** these are *not* documented. The
> harness provides the resolution + dispatch plumbing; sourcing valid live
> pointers from the hosted instance is a separate research step (e.g. resolving
> them from the VST3 EditController's component, or from a one-time read-only
> observation of the GUI Play button's actual `this`). **Never synthesize them.**
> If you do not have verified live pointers, run in DRY-RUN mode.

---

## 4. The exact safety gate

The live internal call requires **all five** of the following. If any is absent,
the driver runs in DRY-RUN mode and makes no call.

| # | Gate | Where enforced |
|---|------|----------------|
| 1 | `MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1` | Driver env check (`check_invoke_gates`) |
| 2 | `OZONE_HEADLESS_INVOKE=1` | Driver env check (distinct private-call opt-in) |
| 3 | CLI `--i-understand-the-risk` | Driver arg check (explicit per-run consent) |
| 4 | `ENABLE_PRIVATE_CALL = true` in `tools/ozone_headless_assistant.js` | In-file flag (operator edits source) |
| 5 | `rpc.invoke()` opts.`enable=true` | Defense-in-depth per call |

In addition, immediately before dispatch the Frida script:
- Re-resolves `iZOzonePro.dll`'s base + size (guards against a relocated/patched
  image between resolve and call).
- Bounds-checks that the vtable thunk and body RVAs land inside the mapped
  module.
- Runs `validateControllerThis()` on the supplied `this`.
- Invokes through the **vtable thunk** `0xD572F0`, never the raw body.

The driver also refuses to attach Frida if the spawned pid's process name
matches any vendor/DAW pattern (`fl64`, `izotope`, `ozone`, `pace`, `ilok`,
`reaper`, `ableton`, `cubase`, `protools`, ...), as a belt-and-suspenders check
that we only ever instrument our own host.

---

## 5. The PACE / anti-tamper caveat (read before any gated call)

The verify verdict flagged this as **ELEVATED / NOT FULLY BOUNDED**:

- The existing `MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1` + `allow_unsafe_write=true`
  gate pair in `src/AI/StandaloneMcp/IZotopeIPCAssistant.cpp` was authored for
  the **shared-memory IPC ring** path. The trigger plan does **not** use that
  ring; it jumps into private, non-exported Ozone code via a `NativeFunction`
  on a vtable slot. So gate #1 above is *necessary but not sufficient* on its
  own — which is why gates #2–#5 exist as a distinct, narrowly-named
  private-call authorization chain.
- `iZOzonePro.dll` loaded inside our own host process is **still the vendor
  module**. Any PACE/iLok wrap or integrity check on that module shares our
  address space. The private call does **not** patch PACE/anti-tamper code
  itself (so it is not a direct anti-tamper write), but it exercises code paths
  and mutates state that licensing/integrity gates may have intended to be
  reachable only through sanctioned entry points. Integrity checks, if present,
  run **in-process** and could react.
- **Confidence that the call is fully benign to PACE/iLok: LOW–MEDIUM.**

If anything unexpected happens (crash, license dialog, integrity warning,
silent state corruption), **stop**, do not retry, and record the observation in
the research log per the methodology Evidence Template.

---

## 6. Verify verdict to honor (in plain terms)

The adversarial verify pass **rejected `0xD58A20` as the confirmed trigger**
(`holds_up: false`, `verdict: rejected`), while confirming:

- The signature (3-arg MSVC thiscall) is correct.
- Vtable-only reachability through thunk `0xD572F0` is correct.
- Master-Assistant cluster membership is correct.
- The "decisive" proof (a phase-staging loop writing immediates 5/6/7/8/9 then
  calling `0xD50740`) was a **misread**: the next instruction after each call
  loads the *same* string "Master Assistant", and `0xD50740` is a TLS-guarded
  index-to-label getter, not a state advancer. The body more likely
  **enumerates Master Assistant module parameters** (strings: "Maximizer",
  "Threshold", "ElementChain", format `"{module}.{param}"`) than starts an
  analysis.

**Implication for the harness:** keep the call gated and treat any output as
*observational*, not as a confirmed analysis-start. The real value of a gated
run is the **parameter delta** it produces — that is the evidence that
distinguishes "start analysis" from "enumerate parameter metadata" from "no-op".
The harness exists so a human can make that one call, once, in a controlled
lab, and read the diff.

---

## 7. What is offline-safe vs. requires the gated call

| Activity | Mode | Safe by default? |
|----------|------|------------------|
| Static recon of on-disk `iZOzonePro.dll` (capstone/pefile/xref) | Offline | Yes (methodology sec 5.2) |
| Hosting Ozone in `MorePhiMcpServer`, rendering audio | Live, own process | Yes (sanctioned host API) |
| `ozone_get_parameters` before/after diff | Live, own process | Yes (sanctioned host API) |
| Frida attach to `MorePhiMcpServer`, resolve RVA, log snapshot | Live, own process | Yes (observe-only) |
| Frida read-only poller hook (`0xEAD3E0`) | Live, own process | Yes (observe-only) |
| **`NativeFunction` call into `0xD572F0`** | **Live, own process** | **NO — requires all 5 gates (Sec 4)** |
| Attaching Frida to FL64 / iZotope / PACE | — | **NEVER (red line)** |
| Touching the applier `0xEAD930` | — | **NEVER (red line)** |

---

## 8. Step-by-step: a DRY-RUN (the default, always-safe path)

```bash
# 1. Build the standalone MCP host (one time)
cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON
cmake --build build --config Release --target MorePhiMcpServer

# 2. Confirm the Ozone Pro VST3 is where the harness expects it
ls "C:/Program Files/Common Files/VST3/iZotope/Ozone Pro.vst3"

# 3. Run DRY-RUN (no env gates set -> the driver will not invoke)
py tools/run_headless_assistant.py \
    --audio path/to/your/track.wav \
    --out-dir tools/live_captures/headless
```

Expected output: `mode: DRY-RUN`, a `trigger_resolved` event with the thunk/body
addresses and a 64-byte disassembly snapshot, a `params_before` baseline, an
`audio_rendered` event, a `params_after_render` diff (the render itself may move
a few params), and a `gate_blocked` event listing exactly which gates are
unsatisfied. **No internal call is made.** `did_not_execute=true`.

---

## 9. Step-by-step: the gated live call (deliberate, human-driven)

> Only do this if you have (a) read Section 5, (b) read Section 6, and
> (c) have a **verified live** `--controller-this`, `--inout-ctx`, and
> `--opts-out` sourced from the hosted instance.

```bash
# 1. Set BOTH environment gates
export MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1
export OZONE_HEADLESS_INVOKE=1

# 2. Flip the in-file master flag (gate #4). Edit the line:
#       var ENABLE_PRIVATE_CALL = false;
#    to:
#       var ENABLE_PRIVATE_CALL = true;
#    in tools/ozone_headless_assistant.js

# 3. Run with explicit consent + live pointers
py tools/run_headless_assistant.py \
    --audio path/to/your/track.wav \
    --i-understand-the-risk \
    --controller-this 0x...   \
    --inout-ctx 0x...         \
    --opts-out 0x...          \
    --out-dir tools/live_captures/headless
```

What happens, in order:
1. Host launches; Ozone loads; Frida attaches to the host pid (name-checked).
2. `rpc.resolve()` resolves and bounds-checks the thunk/body.
3. `rpc.gate_state()` reports the in-file flag.
4. Param baseline captured.
5. Audio rendered through Ozone (arms the signal chain).
6. `check_invoke_gates()` verifies gates #1–#3 + required args.
7. The in-file flag (gate #4) is re-checked.
8. `rpc.invoke({controllerThis, inOutCtx, optsOrOut, enable:true})` dispatches
   through the **vtable thunk** `0xD572F0`.
9. Params captured again; deltas computed.
10. Report written to `<research_id>_report.json`.

**Validate the result against a manual GUI run:**
- Run the same track through Ozone's GUI Play button in a normal DAW session.
- Export the resulting parameter set (via `ozone_get_parameters` against a
  hosted instance in the same state, or via the DAW's preset/state export).
- Compare the GUI-run deltas against the headless-run deltas.
- A match on the *direction* and *approximate magnitude* of the dominant
  parameter moves (e.g. EQ band gains, Maximizer ceiling/threshold) is the
  evidence that the headless call actually started an analysis. A mismatch, or
  no deltas at all, supports the verify verdict's "parameter-metadata
  enumeration" reading.

---

## 10. The "do not attach to FL64" rule (explicit)

This harness **never** attaches Frida to:
- `FL64.exe` / `FL32.exe` / any Image-Line process
- `iZotope.exe` / any `iZ*.exe` product process
- Any PACE / iLok / anti-tamper / integrity-check process
- Any other DAW (`reaper`, `ableton`, `cubase`, `protools`, `studio one`)

The driver enforces this defensively: `attach_frida()` reads the spawned pid's
process name and refuses to attach if it matches any vendor/DAW pattern. The
research that produced the trigger RVA used **only offline static analysis** of
the on-disk DLL (methodology sec 5.2) — no vendor process was ever attached.

If you ever find yourself tempted to attach Frida to a vendor process to
"confirm" the trigger, **stop**. That is a red line. Use the parameter-diff
validation in Section 9 instead.

---

## 11. Files

| File | Role |
|------|------|
| `tools/ozone_headless_assistant.js` | Frida script: resolve, validate, gated invoke via vtable thunk. DRY-RUN by default. |
| `tools/run_headless_assistant.py` | Driver: launch host, attach Frida, render audio, enforce gates, diff params. |
| `docs/OZONE_HEADLESS_ASSISTANT_RUNBOOK.md` | This document. |
| `src/AI/StandaloneMcp/OzonePluginBackend.cpp` | The sanctioned host path that loads Ozone and renders audio. |
| `docs/OZONE_IPC_RESEARCH_METHODOLOGY.md` | The red lines and evidence template this harness honors. |

---

## 12. Evidence template (fill in per gated run)

```
Research ID:        headless-assistant-YYYYMMDD-HHMMSS
Date:
Operator:
Authorization:      (who approved the gated call; which gates were set)
Product/version:    iZotope Ozone Pro (iZOzonePro.dll, base 0x180000000)
Host/version:       MorePhiMcpServer (More-Phi build hash)
OS/build:
Process IDs:        (the MorePhiMcpServer pid ONLY)
Transport:          in-process NativeFunction on vtable thunk 0xD572F0
Capture bounds:     param set before/after; no full-process dump
Stimulus:           --audio <track>; gated invoke of trigger
Observed deltas:    (from <research_id>_report.json)
Schema version:     n/a (host-API param diff)
Excluded observations: PACE/iLok, licensing, integrity-check regions NOT touched
Result:
Next action:
```
