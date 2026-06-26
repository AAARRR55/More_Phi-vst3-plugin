# SonicMaster v2 → VST3 Realtime Neural Mastering Integration

**Date:** 2026-06-21 (updated 2026-06-23)
**Status:** Implemented — ONNX export solved, in-process inference active, HTTP fallback retained
**Source model:** `sonicmaster-v3-decision-engine-20260530T121536Z` → `models/v3/mastering-brain-v2-fullchain-best/checkpoints/best.ckpt`
**Target host:** More-Phi VST3 (`src/`), `AutoMasteringEngine` realtime DSP chain

> **2026-06-23 update:** The ONNX export blocker (§2) has been resolved.
> `tools/export_onnx/export_patched.py` patches three ONNX-incompatible ops:
> (1) `nn.TransformerEncoder` → manual Multi-Head Attention with identical
> state-dict, (2) `torch.fft.rfft`/`rfftfreq` → STFT-based spectral injection,
> (3) `torch.stft(return_complex=True)` → `return_complex=False` with manual
> magnitude. Parity verified (max abs diff 4.01×10⁻⁵). The plugin now uses
> ONNX in-process inference as primary, with HTTP server as fallback
> (`MorePhiProcessor::initializeSonicMaster()`). See `tools/export_onnx/README.md`.

---

## 1. Goal & scope

Feed ~6 s of live audio to the `masteringbrainv2` model, exported to ONNX and run
in-process, on a background thread. Decode its 44-float decision into a safe
mastering-chain plan and continuously apply that plan to the plugin's built-in
`AutoMasteringEngine` DSP chain. The feature ships as a **preview, default OFF**,
with every prediction clamped by the existing `NeuralMasteringSafetyPolicy`.

### Out of scope

- Per-sample / per-block neural audio processing. The model is a
  waveform-to-decision model, not a realtime DSP model; "realtime mastering" here
  means realtime *re-analysis* feeding parameters into a realtime DSP chain.
- Retraining the model or producing a new checkpoint.
- Applying the decision to a hosted third-party plugin (Ozone/Pro-Q). The decision
  JSON is decodable for that path later, but the built-in chain is the only apply
  target in this design.
- Shipping the ONNX model in git. The export is produced offline and staged as a
  resource.

---

## 2. The fundamental mismatch (why this is a new path, not a slot-in)

| Aspect | Existing `OnnxNeuralMasteringRunner` seam | Downloaded `masteringbrainv2` checkpoint |
|---|---|---|
| Format | ONNX (`.onnx`) | PyTorch Lightning (`.ckpt`) |
| Input | 63-float feature vector (LUFS, peak, 32 spectral bands, stereo…) | Raw stereo **waveform**, 262,138 samples (~6 s @ 44.1 kHz), 4 STFT resolutions |
| Output | 72 control deltas in `[-1,1]` | 44-float full-chain decision (8 EQ gains, target LUFS, TP ceiling, 3×6 compressor, 2×2 saturation, 2×2 stereo, char logits) |
| Threading | Message-thread inference, audio-thread-safe | Python/torch offline; needs ~6 s buffered audio per inference |
| Quality status | (seam only) | Research-only; failed 4/9 release gates (EQ MAE 2.12 dB, TP 0.80 dBTP) |

Because the I/O contract is fundamentally different, this is a **parallel path**,
not a modification of the 63→72 seam. The existing `OnnxNeuralMasteringRunner` and
`NeuralMasteringController` are left untouched.

---

## 3. Architecture

A new class, `SonicMasterAnalysisEngine` (in `src/AI/`), owns the entire loop. It
is **not** an `INeuralMasteringModelRunner` — those are synchronous 63-float
planners. This engine is an autonomous analysis loop that owns its own thread, its
own ONNX session, and a waveform-input contract that does not fit the existing
runner interface. It talks to `AutoMasteringEngine` directly via the already
validated `applyValidatedPlan()` handoff.

```
┌─────────────────────────────────────────────────────────────────────┐
│  Audio thread (processBlock)                                         │
│  ┌───────────────┐    memcpy block into      relaxed-atomic          │
│  │ AutoMastering │──► AudioCaptureRing ──►   write-index advance      │
│  │  Engine       │    (lock-free SPSC,        (no locks, no alloc)    │
│  │  (DSP chain)  │    ~8s stereo @ host SR)                          │
│  └──────▲────────┘                                                     │
│         │ applyValidatedPlan(plan)   ◄── ramped param updates         │
└─────────│─────────────────────────────────────────────────────────────┘
          │                                  message thread
══════════│═══════════════════════════════════════════════════════════════
          │
┌─────────┴─────────────────────────────────────────────────────────────┐
│  SonicMasterAnalysisEngine  (owns: ONNX session, AnalysisThread)       │
│                                                                        │
│  AnalysisThread loop (every ~3s):                                      │
│   1. drain AudioCaptureRing → scratch AudioBuffer (latest ~6s)         │
│   2. resample host-SR → 44100 if needed (linear, on this thread)       │
│   3. peak-normalize capture window to a fixed reference level          │
│   4. ONNX session.Run(waveform[1,2,262138] → decision[1,44])           │
│   5. decodeMasteringDecision() → MasteringChainPlan                    │
│   6. NeuralMasteringSafetyPolicy.validate() → ValidatedPlan or reject  │
│   7. push ValidatedPlan to message thread via async message            │
│                                                                        │
│  Message thread (on async callback):                                   │
│   8. ramp DSP params (200ms crossfade) via AutoMasteringEngine setters │
└────────────────────────────────────────────────────────────────────────┘
```

### Key boundaries

- **Audio thread** does *only* a lock-free ring write (memcpy + atomic index).
  No allocation, no locks, no ONNX. Satisfies the project's hard realtime rule.
- **Analysis thread** owns all the heavy work: resample, ONNX inference
  (50–200 ms), decode, safety-validate. It never touches the audio thread and
  only touches ONNX/session state it exclusively owns.
- **Message thread** does the final, cheap parameter ramp (atomic
  `setBand()`-style writes the DSP modules already support). UI never blocks
  because inference isn't here.
- **Teardown order** (critical invariant): on stop, signal the analysis thread
  to exit → join it → *then* destroy the ONNX session.

### What does not change

- `OnnxNeuralMasteringRunner` (the 63→72 seam) is untouched.
- `NeuralMasteringController` is untouched. The new engine sits alongside it,
  both feeding the same `AutoMasteringEngine`.
- The shipped plugin's audio path and reported latency are unchanged when the
  feature is OFF (default).

---

## 4. Components & responsibilities

### 4.1 New files

| File | Role |
|---|---|
| `tools/export_onnx/masteringbrain_to_onnx.py` | **Offline, run once.** Loads the `.ckpt` via the bundled `master_audio.load_module_from_checkpoint()`, wraps the `MasteringDecisionNet` forward in a no-grad trace, exports `masteringbrain_v2_decision.onnx` (opset 17, dynamic batch/sequence axes). Also emits `masteringbrain_v2_contract.json` recording the exact I/O names/shapes/ranges for the runner to validate against at load time. Includes a PyTorch↔ONNX parity check. |
| `src/AI/SonicMasterDecisionRunner.h/.cpp` | Thin ONNX session wrapper for the waveform→decision contract. Owns the `Ort::Session`, input/output name binding, shape validation. Exposes one method: `bool runDecision(const float* stereoInterleaved, std::size_t frames, double sampleRate, float outDecision[kDecisionWidth])`. Reuses the same pimpl + `MORE_PHI_HAS_ONNX` gating pattern as `OnnxNeuralMasteringRunner`. |
| `src/AI/SonicMasterDecisionDecoder.h/.cpp` | Pure, noexcept, ONNX-free: `decodeDecisionToPlan(const float decision[kDecisionWidth], double sampleRate, MasteringChainPlan& out)`. Mirrors `mastering_decision_adapter.decode_mastering_decision()` slice map exactly. Unit-tested directly without ONNX. |
| `src/AI/SonicMasterAnalysisEngine.h/.cpp` | The orchestrator (section 3). Owns the runner, the capture ring, the analysis thread, and the message-thread ramp. |
| `src/Core/AudioCaptureRing.h` | Lock-free SPSC ring buffer (~8 s stereo float, power-of-2 frames). Single producer (audio thread), single consumer (analysis thread). Cache-line-aligned indices, mirroring `LockFreeQueue.h`. |
| `tests/Unit/TestSonicMasterDecisionDecoder.cpp` | Unit tests for the decoder (slice map, range clamping, NaN coercion). |
| `tests/Unit/TestSonicMasterAnalysisEngine.cpp` | Unit tests for the engine with a fake runner (no real model needed). |

### 4.2 Ownership boundaries

**`SonicMasterDecisionRunner`** owns: the ONNX session, input/output tensor
names, the fixed 262138-sample input buffer (pre-allocated in `loadModel` via
embedded BinaryData, reused — zero per-inference allocation). It does *not* own threading, capture,
or resampling. Synchronous `(waveform → 44 floats)` call. Trivially testable
with a stub.

**`SonicMasterDecisionDecoder`** owns: the pure mapping from 44 floats to a
`MasteringChainPlan`. No ONNX, no allocations, `noexcept`. The slice map is the
single source of truth, copied from `mastering_decision_adapter.py` so C++ and
Python agree byte-for-byte:

```
[0:8]  EQ gains (dB)        → AdaptiveEQ bands 0–7 (EQ_DECISION_FREQUENCIES_HZ)
[8]    target LUFS          → LoudnessNormalizer target
[9]    true-peak ceiling    → BrickwallLimiter ceiling
[10]   comp gate            → MultibandDynamics bypass
[11:29] 3×(thr,ratio,atk,rel,makeup,knee) → MultibandDynamics 3 bands
[29]   exciter gate         → HarmonicExciter bypass
[30:34] 2×(drive,mix)       → HarmonicExciter 2 bands
[34]   stereo gate          → StereoImager bypass
[35:39] 2×(width,sideGain)  → StereoImager 2 bands
[39]   limiter aggressiveness → BrickwallLimiter drive
[40]   expected GR (dB)     → telemetry only
[41:44] character logits    → argmax → transparent/balanced/aggressive (telemetry)
```

Every output is clamped to the receiving DSP module's documented range
(e.g. EQ `±12 dB` per `AdaptiveEQ::kMaxGainDB`, limiter ceiling `[-6,-0.1] dBtp`
per schema) and every non-finite value is coerced — defence in depth before the
safety policy even sees it.

**`SonicMasterAnalysisEngine`** owns: the capture ring, the analysis
`std::thread`, the runner, the resample step, the safety-policy instance, and a
pending-plan atomic-flag + timer-callback handoff to the message thread (R5 fix;
replaces old `callAsync` which could drop in headless hosts). It does *not*
own the `AutoMasteringEngine` — it holds a non-owning pointer (set via
`setApplicationEngine()`, mirroring `NeuralMasteringController`).

**`AudioCaptureRing`** owns: one contiguous `std::vector<float>` of capacity
`2 * power_of_two(enough_for_8s @ max SR)`, plus `std::atomic<size_t>`
write/read indices on separate cache lines. The audio thread writes interleaved
stereo and advances the write index; the analysis thread reads and advances the
read index. Capacity is sized so the analysis thread (which reads ~6 s every 3 s)
never laps itself.

### 4.3 Integration into existing code

Only **three** small touch-points in existing files:

1. **`MorePhiProcessor`** (constructor / `prepareToPlay` / `releaseResources` /
   destructor): construct `SonicMasterAnalysisEngine`, wire
   `setApplicationEngine(&autoMasteringEngine_)`, call
   `engine.prepare(sampleRate, blockSize)`, `engine.capture(buf)` inside
   `processBlock`, and `engine.release()` before teardown. The capture call is
   the one new audio-thread hot-path line.
2. **APVTS**: one new boolean parameter `SonicMasterAnalysisEnabled`
   (default `false`). The engine's `setActive()` is bound to it.
3. **`MorePhiEditor`**: one toggle button "Neural Master (Preview)" with a small
   status line ("Analyzing… / Applied / Model unavailable"). Disabled if
   `!runner.isAvailable()`.

Everything else is additive. The existing `NeuralMasteringController`,
`OnnxNeuralMasteringRunner`, MCP server, and the entire DSP chain are untouched.

### 4.4 Reuse, not reinvention

- ONNX session lifecycle: copy the proven pimpl + `MORE_PHI_HAS_ONNX` pattern from
  `OnnxNeuralMasteringRunner`.
- Lock-free ring: copy the cache-line-aligned SPSC structure from `LockFreeQueue.h`.
- Safety validation: reuse the existing `NeuralMasteringSafetyPolicy` verbatim.
- Plan application: reuse `AutoMasteringEngine::applyValidatedPlan()` — it already
  routes to `AdaptiveEQ::setBand()`, `MultibandDynamicsProcessor`,
  `BrickwallLimiter`, etc.
- Parameter ramp: the DSP modules already take atomic parameter writes; the
  message thread just sets them with a short `juce::SmoothedValue` ramp.

---

## 5. Data flow & control flow

### 5.1 Steady-state cycle (every ~3 s)

**Audio thread — every block (`processBlock`):**

```
if (engine.active_)                      // atomic relaxed read
    captureRing_.write(buf.getReadPointer(0),
                       buf.getReadPointer(1),
                       buf.getNumSamples());   // memcpy + atomic write-index advance
```

That is the entire audio-thread cost when ON: two `memcpy`s + one relaxed atomic
store. When OFF: a single atomic load + early return. Zero allocation, zero
locks, `noexcept`.

**Analysis thread — waits on a `std::condition_variable` pinged by a 3 s timer
(or on `requestStop`):**

1. **Drain**: read the most recent `kDecisionSegmentFrames` (262138) frames from
   the ring into a pre-allocated scratch buffer `captureScratch_[2][262138]`. If
   fewer than the required frames have been captured since `prepare()`, skip this
   cycle.
2. **Resample**: if `hostSampleRate != 44100`, linear-resample into a second
   pre-allocated buffer `modelScratch_[2][262138]` at 44.1 kHz. If already 44.1,
   this is a no-op alias. (Linear interp matches the engine's own
   `preprocess_audio` in `api.py`, so behaviour is consistent with how the model
   was validated.)
3. **Normalize gain**: scale the captured window so its peak sits at a reference
   (e.g. −1 dBTP) before inference. Rationale: the model was trained on a corpus;
   feeding wildly different operating levels shifts its predictions. Reversible
   because we only read *parameters* out, not audio. Captured and logged for
   telemetry; does not affect output audio level (the DSP chain operates on the
   real, unscaled stream).
4. **Inference**: `runner_.runDecision(modelScratch_, 262138, 44100.0,
   decisionOut_)` → fills `decisionOut_[44]`. Typical wall time 50–200 ms on CPU;
   this thread has no realtime budget so it is free to take as long as it needs.
5. **Decode**: `decodeDecisionToPlan(decisionOut_, sampleRate, chainPlan_)` →
   `MasteringChainPlan`. All NaN/Inf coerced, all ranges clamped.
6. **Safety**: build a `NeuralMasteringPlanCandidate` from `chainPlan_` + a
   conservative confidence (0.85), then
   `safetyPolicy_.validate(candidate, runtimeState_)`. If `accepted`, proceed; if
   `fallbackSelected`, hold the last safe plan and skip the apply (do not push a
   rejected plan downstream).
7. **Hand off**: store plan in `pendingPlan_`, set `pendingApplication_` atomic
   flag (R5 fix — replaces `callAsync`). The processor's timer callback polls
   `hasPendingApplication()` and calls `processPendingApplication()` on the
   message thread, which invokes `applyRamped(plan)`. The analysis thread is
   immediately free for the next cycle.

**Message thread — `applyRamped(plan)`:**

8. Call `autoMasteringEngine_->applyValidatedPlan(plan)` once — this is the single
   apply surface. It updates the engine's `lastSafeNeuralPlan_` bookkeeping and
   sets all target parameters. Parameters apply instantaneously (the dead
   `rampDurationSeconds` config field was removed in R8).

### 5.2 Cadence & timing constants

| Constant | Value | Rationale |
|---|---|---|
| Capture ring capacity | 8 s @ max supported SR, power-of-2 frames | ≥ the 6 s model window with headroom so the analysis thread never overruns |
| Analysis interval | 3 s | Re-analyze roughly twice per model window; balances responsiveness vs CPU. Configurable via APVTS later. |
| Param ramp duration | 200 ms | Long enough to be click-free across any parameter jump, short enough to feel responsive |
| Required frames before first inference | 262138 @ 44.1k (≈6 s) | The model's hard input requirement; never feed a short/padded window during the first seconds after activation |
| Confidence floor | 0.85 | Above the safety policy's default `minConfidence` of 0.75, so a flaky preview model clears the gate on plausible frames while the safety policy still rejects garbage |

### 5.3 Activation flow

User toggles "Neural Master (Preview)" ON:

1. Message thread: `engine.setActive(true)` → atomically flips `active_`. Audio
   thread begins capturing on the next block.
2. Message thread: spawns the analysis thread if not running, pings its condition
   variable.
3. The first ~6 s, the UI status reads "Collecting audio…" (engine reports
   `capturedFrames < required`).
4. First successful inference → "Applied (plan #N)". Subsequent cycles refresh
   every 3 s.

User toggles OFF:

1. `engine.setActive(false)` → audio thread stops capturing (early return).
2. Analysis thread is **not** killed (cheap to keep idle) but its loop no-ops
   until re-enabled. The current DSP parameters are *held* (not reset) so the
   master setting persists.

### 5.4 Concurrency invariants

- **SPSC discipline on the ring**: only the audio thread writes the write-index,
  only the analysis thread writes the read-index. Both read both indices, but
  each owns one. Modeled exactly on the existing `LockFreeQueue.h`.
- **ONNX session is analysis-thread-exclusive**: created in `prepare()` (message
  thread), used only inside the analysis-thread loop, destroyed in `release()`
  (message thread) *after* joining the analysis thread. No other thread ever
  touches it.
- **Message-thread handoff is the only analysis→message crossing**: via
  pending-plan atomic flag + timer callback (R5 fix — replaces `callAsync`).
- **`active_` is atomic**: audio thread reads it relaxed on every block; message
  thread sets it. A toggle takes effect within one block.

---

## 6. Error handling & failure modes

Principle: **when in doubt, hold the last safe state and keep the DSP chain
running untouched.**

### 6.1 Failure → response matrix

| Failure | Detection | Response | DSP/audio impact |
|---|---|---|---|
| ONNX not linked (`MORE_PHI_ENABLE_ONNX=OFF`) | `runner_.isAvailable()==false` at construction | UI toggle disabled + greyed, status "Model unavailable (build without ONNX)". Engine never spawns analysis thread. | None |
| ONNX linked but model load fails | `loadModel()` returns false (model embedded via BinaryData; load validates I/O shapes) | Same as above; logged once. `active_` cannot be set true. | None |
| Model I/O shape mismatch at load | `loadModel()` validates input `[?,2,262138]` + output `[?,44]`; rejects on mismatch | Load fails → unavailable. Guards against silently swapping in an incompatible checkpoint. | None |
| Insufficient audio captured | `capturedFrames < 262138` at cycle start | Skip cycle, status "Collecting audio…" | None — DSP holds current params |
| Inference throws / returns error | try/catch around `session.Run()` | Log, skip cycle, keep last safe plan. After 3 consecutive failures, auto-disable the feature and surface "Analysis error — see log". | None — last safe params held |
| Model outputs NaN/Inf | Decoder coerces to 0 before building the plan; `sanitizePlanCandidate()` is the second line | Treated as all-zero deltas → effectively "no change" | None |
| Model outputs out-of-range values (e.g. EQ +20 dB) | Decoder clamps to `±kMaxGainDB`; safety policy re-clamps | Plan is clamped, applied | Gentle, bounded |
| Safety policy rejects (low confidence, stale, max-delta) | `validate()` returns `!accepted` | Hold last safe plan; do not push to message thread | None |
| Runtime overload (xrun) | `NeuralMasteringRuntimeState::overload` from `processBlock` | Analysis thread reads it, skips cycles while set, lowers effective cadence to 6 s until overload clears | Self-throttling, no glitch |
| Unsupported sample rate (e.g. 192k) | `runner_` reports supported SR set from `contract.json`; if host SR not in set and resample target (44.1k) is supported, resample; else unavailable | Feature unavailable at that SR | None |
| Host changes sample rate / block size mid-session | `prepareToPlay` called again | `engine.prepare()` re-sizes ring + scratch buffers, resets captured-frame counter, joins + restarts analysis thread cleanly | Brief "Collecting audio…" gap, no audio drop |
| Mono or >2 channel audio | `buf.getNumChannels()` check in capture | Mono → duplicate to stereo before ring write. >2 → take first two channels. Logged once. | None |
| Teardown while analysis thread mid-inference | `release()` sets `requestStop`, notifies CV, **joins** thread, then destroys session | Analysis thread checks `requestStop` at loop top and between safe steps; exits cleanly | None |
| Plugin state save/restore | `setStateInformation` | Enabled state restored via APVTS. Last applied plan not persisted (re-derived from audio on resume). | Re-collects audio after restore |

### 6.2 Three always-hold invariants

1. **Audio thread is never blocked.** Capture is a memcpy + relaxed atomic. No
   lock, no allocation, no syscall. Even if the analysis thread is wedged or ONNX
   is hung, the audio thread is unaffected.
2. **The DSP chain is never left in an undefined state.** Either a validated plan
   is applied (ramped), or the last safe plan is held, or — on first activation
   with no plan yet — the chain keeps its current parameters. No code path writes
   half a plan.
3. **The ONNX session outlives every thread that touches it.** `release()` joins
   the analysis thread before the session destructor runs. Checked by a
   debug-only assertion and by the teardown-ordering test.

### 6.3 Accepted limitations (documented, not hidden)

- **Cannot react faster than ~3–6 s.** A sudden transient or level change will
  not be re-mastered until the next cycle. Inherent to a waveform-to-decision
  model with a 6 s window. The DSP chain's own realtime protection (limiter,
  normalizer) remains the first line of defence during the gap.
- **Not sample-accurate.** Produces static parameter settings, not a time-varying
  control signal. The 200 ms ramp is the only automation.
- **Can be wrong.** EQ MAE 2.12 dB and true-peak 0.80 dBTP mean on some material
  it will overshoot. Safety clamps bound the damage; "Preview" labeling and
  default-OFF posture set expectations. Research-grade assistant, not an autocrat.

These limitations are documented in the UI tooltip and the README, not hidden.

### 6.4 Logging & observability

Every cycle logs (throttled): plan ID, inference wall-time, per-group
clamped-vs-raw delta counts, safety verdict. The UI status line surfaces the last
verdict ("Applied #12" / "Held #11 (low confidence)" / "Collecting audio…").

---

## 7. Testing strategy

### 7.1 Tier 1 — Pure unit tests (no ONNX, no model)

**`TestSonicMasterDecisionDecoder`** — load-bearing correctness tests.
- Slice-map exactness: known 44-float vector → each slot lands in the right DSP
  target.
- Range clamping: `+20.0f` EQ → clamped to `±kMaxGainDB`; `-100.0f` TP → clamped
  to `[-6,-0.1]`.
- NaN/Inf coercion: NaN at every slot → each becomes the module's neutral value,
  no exception.
- Character argmax: logits `[0.1, 0.9, 0.2]` → "balanced".
- Golden-vector regression test on one fixed input.

**`TestAudioCaptureRing`** — concurrency correctness tests.
- SPSC discipline: producer/consumer data match.
- Wraparound: power-of-2 boundary handled correctly.
- Overflow: consumer reads the newest window, never corrupt data.
- No-allocation assertion via the project's `AllocationTracker`.

### 7.2 Tier 2 — Engine integration tests (fake runner)

**`TestSonicMasterAnalysisEngine`** — uses a `StubDecisionRunner` returning a
canned 44-float decision, no ONNX.
- End-to-end happy path: capture → trigger → decode → safety → async handoff →
  `applyValidatedPlan` on a fake `AutoMasteringEngine`.
- Insufficient-audio skip.
- Safety-reject hold.
- 3-consecutive-failure auto-disable.
- Activation/deactivation: capture starts/stops, analysis thread idle-but-alive,
  DSP params held (not reset) on OFF.
- Teardown ordering: `release()` mid-cycle → analysis thread joined before
  session destroyed (instrumented via destructor-ordering flag).

### 7.3 Tier 3 — ONNX live-inference smoke test (opt-in)

**`TestSonicMasterRunnerLive`** — only compiled when
`MORE_PHI_ENABLE_ONNX=ON` *and* a staged model file is present
(`masteringbrain_v2_decision.onnx`), mirroring `TestOnnxNeuralMasteringRunner`.
- Load the real exported model.
- Feed a synthetic 6 s sine+noise sweep → assert 44 finite floats in range.
- Assert inference completes in < 500 ms.
- Verification gate for the export script; does not assert subjective quality.

### 7.4 Export-script parity check

`masteringbrain_to_onnx.py` is itself verified:
- Export the checkpoint, run the same 6 s waveform through both PyTorch
  `MasteringDecisionNet.forward()` and the ONNX graph.
- Assert per-output max absolute difference < 1e-4.
- Emit `export_onnx/parity_report.json`. If parity fails, the export is broken;
  the runner refuses to load a model whose `contract.json` is absent.

### 7.5 Manual / subjective verification (documented, not automated)

- Load in a DAW, enable preview, play reference tracks of known genres, confirm
  the chain moves sensibly (bright material tamed, quiet material lifted).
- A/B against the offline `api.py /predict` + `apply_chain` render on the same
  track: in-plugin result should be close (same model, same decision format),
  modulo the realtime DSP chain's differing topology.
- Confirm zero glitches / xruns during a 10-minute session on minimum-spec.

### 7.6 Build & CI wiring

- New tests added to `tests/CMakeLists.txt` linked against the shared-code target
  (Tier 1 + 2 always built; Tier 3 only under `MORE_PHI_ENABLE_ONNX=ON`).
- The ONNX model file is **not** committed to git. `tests/CMakeLists.txt` stages
  it via the same mechanism as `model_scaled.onnx` when present, skips the Tier 3
  test when absent.
- The export script lives in `tools/export_onnx/` and is run manually
  (quickstart-documented), producing the model + `contract.json` that get staged
  for Tier 3 and shipped with the plugin's resources.

---

## 8. Open items deferred to implementation planning

- Exact `MasteringChainPlan` struct shape (whether it reuses
  `ValidatedNeuralMasteringPlan` directly or adds a small adapter).
- Whether `kDecisionWidth` (44) and the slice offsets live in
  `SonicMasterDecisionDecoder.h` or `NeuralMasteringTypes.h`.
- Precise APVTS parameter ID string and layout-tree placement for the toggle.
- Whether the message-thread ramp uses `juce::SmoothedValue` per target or a
  single `juce::Timer` driving a normalized 0→1 scalar.
