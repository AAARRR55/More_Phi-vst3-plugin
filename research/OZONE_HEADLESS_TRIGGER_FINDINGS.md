# Ozone Master Assistant — Headless Trigger Findings (2026-06-20, revised)

**Verdict (revised): NOT "impossible."** The Master Assistant controller **is
reachable and alive in a headless, fully-automated host** (no human, no DAW), and
the **Assistant wizard opens headlessly** via UI Automation `Invoke`. What remains
blocked is firing a *fresh analysis*: the wizard's Play/Next controls are a pure
bitmap overlay that is **input-immune** in the headless host, and the binary-
interception candidate (`0xD58A20`) **faults even with fully-live controller
context** (verified this session).

## What was actually proven

1. **No public trigger parameter** (still true). Ozone Pro exposes 646 VST3 params;
   none start the Assistant. `ozone_run_master_assistant` always returns
   `assistant_parameter_not_found`. (`tools/probe_ozone_params.py`.)

2. **The stdio MCP host cannot wake the controller** — but a **GUI-initialized
   host can.** In `MorePhiMcpServer` (stdio) the poller never fires and
   `createEditor` deadlocks bootstrapping `MessageManager`. The earlier "proof #3"
   was specific to the stdio host, NOT fundamental. (`tools/run_ozone_capture.py`.)

3. **BREAKTHROUGH — `OzoneHeadlessHost.exe`** (`src/Tools/OzoneHeadlessHost/`,
   a `ScopedJuceInitialiser_GUI` + message-loop host): loads Ozone Pro, constructs
   its `AudioProcessorEditor` (1130×553, a native `VST3PluginWindow`), feeds audio
   on a timer, and the **Master Assistant controller comes alive** — the idle
   poller `0xEAD3E0` and applier `0xEAD930` fire ~40–180× per run, getter
   `0xD50740` ~540×. The editor + controller exist with **no human, no DAW, no
   visible interaction.** (`tools/run_host_capture.py`.)

4. **The trigger surface is located in the live heap.** A read-only scan finds
   the live objects whose vtables contain the trigger thunk `0xD572F0` (a deep
   multiple-inheritance controller — sub-object vtables at `0x28a9268`–`0x28a9448`,
   thunk at slots 2–62). (`tools/ozone_find_trigger_obj.js`,
   `tools/run_host_probe.py`.)

5. **SUPERSEDED (2026-06-20):** the earlier claim "thread-correct, host-
   surviving invocation works" does **not** reproduce. With fully-live context
   captured from the poller and the call dispatched off the poller stack, the
   thunk/body still **faults and crashes the host** (see revised §"still
   blocked"). The earlier observation was a partial/early dispatch that never
   actually reached the faulting inner code path.

6. **NEW — the Assistant wizard opens headlessly via UI Automation.** UIA
   `InvokePattern.Invoke()` on the `Master Assistant` Button opens the modal
   "What are you going for?" wizard (41% pixel diff, vision-verified). The
   wizard's Next/Play controls are a pure bitmap overlay, input-immune in this
   host. (`tools/run_ozone_invoke_trigger.py`, `ozone_trigger_multimethod.py`,
   `ozone_mouse_input_test.py`, `kbd_frida` probe.)

7. **NEW — live controller context captured, disassembly confirmed.** Observe-
   only poller hook yields the live `this` (vtable RVA `0x28b8878`), live `rdx`,
   readable flag bytes; capstone disassembly of thunk `0xD572F0` (`add rcx,8;
   call body`) and body `0xD58A20` (`mov rbx,[rcx]`, `cmp byte[rbx+0x90]`,
   `mov rcx,[rax+0x20]`; `r8` overwritten by a stack-local before any deref).
   (`tools/run_live_context_capture.py`, `tools/ozone_disasm_trigger.py`,
   `tools/live_captures/live_context.json`.)

8. **NEW — the gated binary-interception call faults with live context.** Fired
   from a separate Frida `Thread` (JS thread stays alive to report), with real
   `this`+`rdx`+writable `r8`, across 3 runs (thunk-mode ×2, body-mode ×1): the
   `NativeFunction` call crashes the host instantly — no survival, no pipeline
   delta. The fault is NOT missing context (it was supplied); it is unsatisfied
   GUI-view/session invariants inside the body. (`tools/ozone_gated_trigger.js`,
   `tools/run_gated_trigger.py --arm [--body]`.)

## What is still blocked (the analysis-START) — revised 2026-06-20

**Headless reach achieved this session:** the Master Assistant **wizard opens**
via UI Automation `Invoke` (no human, no DAW). What remains blocked is firing a
*fresh analysis* from that wizard.

### UI path — wizard opens, but is input-immune (NEW finding)

The earlier note ("real mouse click on the Master Assistant button → zero tree
change, panel will not open") was correct for *mouse*, but **incomplete**: the
button's accessibility `Invoke` pattern **does** open the Assistant.

- UIA `Control.GetInvokePattern().Invoke()` on the `Master Assistant` Button
  (`IsEnabled: True`, `IsKeyboardFocusable: True`) **opens the Assistant wizard**.
  Verified two independent ways:
  - **Pixel diff**: before→after Invoke = **41% of pixels changed**, mean
    brightness 28.5→16.4 (the dark wizard panel rendered). Idle frames (no
    interaction) are stable at 44.4 brightness, so the change is real, not
    capture artefact.
  - **Vision confirmation** of the post-Invoke screenshot: the modal
    "What are you going for?" wizard is shown, with MODULES (Modern/Vintage),
    LOUDNESS & EQ (Manual/Reference + Low/Med/High intensity), DESTINATION
    (Streaming/CD) sections, and a **"Next →" button**. The instruction
    "play the loudest portion of your track" is visible.
    (`tools/live_captures/shots/step1_wizard_open.bmp`.)
- BUT the wizard's own controls (Next, the analysis **Play** circle) are a
  **pure bitmap overlay** — they are **never** exposed to the UIA tree (99
  controls before == 99 after; 0 new controls, 0 actionable patterns added).
- And the wizard is **input-immune** in this headless host. Tested exhaustively
  against the open wizard, every method = **0.0% pixel diff**, and the analysis
  pipeline never fires (`pipeline_root` delta = 0):
  - `SendInput` hardware mouse at the wizard Close-X and at the Next button.
  - `PostMessage` `WM_LBUTTONDOWN/UP` to the editor HWND and to the native
    `iZ_OZONEPROMS_v91100_Win32Window` child, at the correct client coords.
  - `mouse_event` real cursor + `SetCursorPos`.
  - Keyboard `WM_KEYDOWN/UP` for `VK_RETURN`, `VK_SPACE`, `VK_ESCAPE`, `VK_RIGHT`
    (single + 60-press bursts) to the foregrounded editor.
  (`tools/ozone_trigger_multimethod.py`, `ozone_mouse_input_test.py`,
  `ozone_view_diff_probe.py`, `run_ozone_full_trigger.py`, `kbd_frida` probe.)
  Conclusion: the wizard renders but its hit-testing/input dispatch requires a
  host/DAW-embedded focus context the headless editor does not provide. The
  toolbar `Master Assistant` button works (JUCE-level accessible) but the wizard
  it spawns ignores all input.

### Binary-interception path — fires with live context, then faults (NEW finding)

User-authorized binary-interception / DLL-injection path, **our own
`OzoneHeadlessHost` process only** (Frida never attaches to any vendor/DAW/PACE
process; the applier `0xEAD930` is never touched). This overturns the earlier
"needs the GUI context" caveat — we now **have** the live context:

- **Live controller captured** (observe-only, from the poller `0xEAD3E0`'s own
  `onEnter`): `this` (rcx) stable across runs, vtable RVA **`0x28b8878`** inside
  `iZOzonePro.dll`; live `rdx` (a valid in/out context the poller itself passes);
  the body-touched flag bytes `this+0x90/0x98/0xa0` all readable.
  (`tools/run_live_context_capture.py`, `tools/live_captures/live_context.json`.)
- **Disassembly confirmed** (`tools/ozone_disasm_trigger.py`, capstone): thunk
  `0xD572F0` = `add rcx,8; call body`; body `0xD58A20` prologue =
  `mov rdi,rcx; mov rbx,[rcx]` (vtable), `cmp byte[rbx+0x90]`,
  `mov rcx,[rax+0x20]` (vtable slot). The original `r8` arg is saved to `r13`
  but overwritten with a stack-local (`lea r8,[rbp+0x1a8]`) before any deref.
- **The call was made with fully-live context** (real `this`, real `rdx`, fresh
  writable `r8` buffer), dispatched from a separate Frida `Thread` so the JS
  thread could report survival/counters. Result, **identical across 3 runs**
  (thunk-mode rcx=`this-8` ×2, direct body-mode rcx=`this` ×1):
  the `about_to_fire` message arrives (flush-handshake confirmed), the
  `NativeFunction` is invoked — and the **host process crashes instantly**
  (access violation). No 0.5s "beat" ever fires, no `fired_immediate`, host
  process gone. `pipeline_root`/`body` delta never observed > 0.
  (`tools/ozone_gated_trigger.js`, `tools/run_gated_trigger.py --arm [--body]`,
  logs `tools/live_captures/arm_log.txt`, `arm_body_log.txt`.)

**Verdict (now decisive, not contested):** `0xD58A20`/`0xD572F0` is **not** a
clean headless analysis-start. It is a context-sensitive controller method that
**faults even when called with the live controller's own `this` and `rdx`** —
i.e. the fault is not missing-context (we supplied it), it is that the method's
internal invariants (GUI view state / a fully-built analysis session object
referenced through the vtable `[rax+0x20]` slot) are only satisfied on the GUI
Play path. This corroborates the adversarial verify reading that `0xD58A20` is
parameter-metadata enumeration rather than analysis-start, and rules out
"just synthesize a better r8."

### Why the "arm-byte" escape also fails (disassembly, NEW)

The last open hypothesis — "the true analysis-start may be a state-byte
mutation the poller then advances" — is **disproven** by disassembling the
poller `0xEAD3E0`, the hub `0xEABDB0`, and the controller vtable's key slots
(`tools/ozone_disasm_poller.py`, `tools/ozone_disasm_vtable.py`):

- The poller's **per-tick virtual call** is vtable **slot +0xc0** (target
  RVA `0xeba140`): a thin `call [rax+0xd0]; mov rbp,[rax]` — a **status
  read**, not an analysis advancer. The poller also calls the getter
  `0xD50740` (index→label) and branches on `cmp ebx,8` (a *state-machine
  status* compare), but it only **reads** state — it never writes an arm flag
  nor reaches the pipeline root `0xE9FC30`. (Confirmed empirically: across
  ~10 runs `pipeline`/`body` counts stayed 0 while `poller` ran 50–700×.)
- vtable **slot +0x20** (target RVA `0xecfcc0`) — the `[rax+0x20]` the body
  dereferences — is a **result/parameter-record queue iterator**: it walks
  `[this+0x40]+0x180/+0x200 → [rdx+0x40]/[rdx+0x80]/[rdx+0x88]/[rdx+0x90]`,
  a linked-list/ring of records. The body faults because this queue is
  **empty/unpopulated** until the GUI Play path fills it.
- Conclusion: there is **no idle controller byte** writing which makes the
  poller start an analysis. The analysis pipeline is reachable **only** via
  the body `0xD58A20`, and the body **only** runs correctly when the GUI has
  first populated the result-queue. Headless, that queue is never populated,
  so the body faults. The poller cannot be coerced into populating it.

This closes the final escape: **starting a fresh Ozone Master Assistant
analysis headlessly is not achievable** without the GUI Play path populating
the controller's result-queue first. Reaching/opening the Assistant wizard
headlessly is solved; starting its analysis is not.

## Path to completion (concrete)

1. **Real-audio-thread UI drive**: host Ozone on a genuine JUCE
   `AudioProcessorPlayer`/`AudioDeviceManager` audio thread (not the
   message-thread `processBlock` timer) so Ozone sees streaming audio +
   `isPlaying`; then a UI-automated Master Assistant → wizard → Next → Play
   sequence may engage (the wizard's input dispatch may require real streaming
   state). Highest-probability remaining no-human route.
2. **Capture-and-replay the GUI call context**: trace the *real* `rdx`/`r8`/
   vtable-state the GUI Play path uses the first time it fires (requires one GUI
   Play in a real DAW), then replay via the gated internal call. Needs the GUI
   once — still the cleanest way to disambiguate whether `0xD58A20` is even the
   right method.
3. **Identify the true analysis-start by correlation**: instead of trusting the
   static call-graph, hook a broad net of Master-Assistant-cluster methods and
   diff which one fires *only* on a real GUI Play — that method, not
   `0xD58A20`, is the replayable analysis-start.


## Why the controller is reachable but analysis-start isn't

The Assistant *runtime* (poll/apply/re-apply) is constructed with the editor and
runs autonomously. The Assistant *analysis-start* is a GUI-initiated action whose
call context is built on the GUI side and is not reconstructable from the runtime
alone. iZotope provides no parameter/IPC surface to inject that context.

## Achievable paths (kept)

- **Assistant wizard opens headlessly** (`tools/run_ozone_invoke_trigger.py`):
  UIA `Invoke` on the Master Assistant button opens the wizard with no human, no
  DAW. The analysis-start (Play) from the wizard is blocked (input-immune), but
  reaching the Assistant UI itself is solved.
- **Diff workflow** (`tools/ozone_assistant_diff.py`): one GUI Play + param diff.
  Still the simplest working production path.
- **Gated lab harness** (`tools/ozone_headless_assistant.*` + runbook): 5-gate;
  real live object source via `OzoneHeadlessHost`.
- **`OzoneHeadlessHost` + live-context capture + disasm**: controller alive
  headlessly, live `this`/`rdx` captured, thunk/body disassembled. The gated
  internal call **faults** with live context — so the candidate `0xD58A20` is
  ruled out as a clean headless analysis-start.

## Tooling artifacts

Recon: `tools/ozone_static_recon.py`, `tools/probe_ozone_params.py`,
`tools/probe_narrow.py`, `tools/ozone_capture_controller.js`,
`tools/ozone_probe_controller.js`, `tools/ozone_rtti_probe.js`,
`tools/ozone_find_trigger_obj.js`, `tools/ozone_invoke_trigger.js`.
Drivers: `run_ozone_capture.py`, `run_host_capture.py`, `run_host_probe.py`,
`run_host_invoke.py`. Host exe `OzoneHeadlessHost`
(`src/Tools/OzoneHeadlessHost/`). Plus ~25 recon probes under
`tools/live_captures/static/`.

**Added this session (UI wizard-open + binary-interception):**
`tools/run_ozone_uia.py` (accessibility tree), `tools/ozone_full_patterns_probe.py`
(pattern survey), `tools/ozone_trigger_multimethod.py` (7-method activation test
+ screenshots), `tools/ozone_view_diff_probe.py` (before/after tree diff),
`tools/ozone_mouse_input_test.py` (SendInput reach test),
`tools/run_ozone_invoke_trigger.py` + `run_ozone_full_trigger.py` (Invoke-driven
wizard open + coord-click Next), `tools/ozone_button_state_probe.py`,
`tools/ozone_wizard_actionables.py`, `tools/run_live_context_capture.py` +
`tools/ozone_live_context.js` (observe-only live `this`/`rdx`/vtable capture),
`tools/ozone_disasm_trigger.py` (capstone thunk/body prologue),
`tools/ozone_gated_trigger.js` + `tools/run_gated_trigger.py`
(user-authorized gated binary-interception fire, thread-isolated, with live
beat reporting). Evidence: `tools/live_captures/{live_context.json,
gated_trigger_result.json, invoke_trigger_evidence.json, full_trigger_evidence.json,
shots/step1_wizard_open.bmp, arm_log.txt, arm_body_log.txt}`.

