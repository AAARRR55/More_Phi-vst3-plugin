# Technical Audit Report: Neural Model-Based Mastering System (More-Phi VST3 Plugin)

**Audit Date:** 2026-07-16  
**Audited Version:** 3.4.1  
**Auditor:** Senior Audio Software Engineer (AI Assistant)  
**Scope:** SonicMaster realtime neural mastering path — ONNX model inference, VST3 plugin hosting, AI assistant integration, mastering chain application, and end-to-end signal flow.

---

## 1. Executive Summary

The SonicMaster neural mastering system is a **functionally complete but partially dormant** pipeline. The ONNX inference path, safety policy, and AI assistant integration are **architecturally sound and extensively hardened** following a recent audit remediation cycle (2026-06-25 through 2026-07-16). However, the **internal DSP mastering chain is intentionally inactive** in the shipped plugin (`intelligenceActive_ = false`), meaning all neural decisions reach audio **only through the hosted-plugin (Ozone) bridge**. This is a design choice, not a bug, but it creates a critical operational dependency: the Ozone parameter map must be populated via `audit_ozone_parameters` before any neural decision becomes audible.

**Overall Assessment:** The system is **production-ready for the hosted-plugin path** with robust safety guards, structured diagnostics, and closed-loop feedback. The **internal DSP chain is dormant** and requires activation to be a standalone mastering solution. Several **measurement blind spots** remain in the analysis engine (no noise floor, SNR, harshness, muddiness). A **priority-ordered remediation list** is provided at the end of this report.

---

## 2. Layer 1: Neural Model Performance & Correctness

### 2.1 What Is Working Correctly

#### 2.1.1 ONNX Input Layout Fix (AUDIT-FIX A1) — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionRunner.cpp:188-194`

The runner correctly de-interleaves stereo input into `[1, 2, N]` row-major layout before feeding the ONNX session. The previous code did a verbatim `std::copy_n` of interleaved L0,R0,L1,R1... into a tensor declared as `[1,2,N]`, which placed all samples into channel 0 with channel 1 reading garbage. The fix uses a two-pass copy: all L samples into `dst[0..N-1]`, all R samples into `dst[N..2N-1]`.

```cpp
for (std::size_t t = 0; t < N; ++t)
{
    dst[t]     = stereoInterleaved[2 * t + 0];  // channel 0 = all L
    dst[N + t] = stereoInterleaved[2 * t + 1];  // channel 1 = all R
}
```

#### 2.1.2 Contract Validation at Load Time (AUDIT-FIX A2) — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionRunner.cpp:49-64`, `src/AI/SonicMasterDecisionRunner.h:72-81`

The runner validates a sibling `.contract.json` file at `loadModel()` time, comparing `schema`, `input_layout`, `normalization`, `dtype`, `sample_rate`, `segment_frames`, and `peak_target_linear` against compile-time constants. A model retrained with different preprocessing is rejected at startup rather than producing silently-wrong decisions.

```cpp
bool validate() const noexcept
{
    return schema == kSonicMasterContractSchema
        && inputLayout == kSonicMasterInputLayout
        && normalization == kSonicMasterNormalization
        && dtype == kSonicMasterDtype
        && std::abs(sampleRate - kSonicMasterModelSampleRate) < 0.5
        && segmentFrames == kSonicMasterSegmentFrames
        && std::abs(peakTargetLinear - kSonicMasterPeakTargetLinear) < 1e-4f;
}
```

#### 2.1.3 I/O Shape Validation with Rank Checking (AUDIT-FIX A1) — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionRunner.cpp:121-135`

The runner validates input rank (3), batch dimension (1 or -1), channel dimension (2 or -1), and time dimension (exactly `kSonicMasterSegmentFrames` or -1). It also validates total element counts. The code uses `GetDimensions()` instead of `GetShape()` to avoid an ORT 1.22.1 segfault with symbolic batch dimensions.

#### 2.1.4 Peak Normalization Before Inference (AUDIT-7) — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:588-609`

Captured audio is peak-normalized to `-1 dBFS` (`kSonicMasterPeakTargetLinear = 0.89125094f`) before inference. The normalization uses the named contract constant, not a bare literal. This is consistent with the training preprocessing, ensuring the model sees a consistent operating level.

```cpp
const float gain = kSonicMasterPeakTargetLinear / peak;
for (std::size_t i = 0; i < kSonicMasterSegmentFrames; ++i)
{
    interleaved_[2 * i + 0] = modelL_[i] * gain;
    interleaved_[2 * i + 1] = modelR_[i] * gain;
}
```

#### 2.1.5 Polyphase Resampling (Host → 44.1 kHz) — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:38-101`

The analysis engine uses a 128-phase × 8-tap polyphase FIR (Kaiser window, β=6, 64-tap prototype, ~60 dB stopband) for host-rate to 44.1 kHz resampling. This runs on the analysis thread (~0.3 Hz), not the audio thread, so the CPU cost is acceptable. The resampler is deterministic and zero-allocation after the first call.

#### 2.1.6 Silent Input Rejection — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:594-602`

If the captured peak is `< 1e-15f` or non-finite, inference is skipped and `DecisionFailure::SilentInput` is reported. This prevents passing all-NaN or all-zero buffers to the model, which would produce undefined behavior in softmax/layer-norm operators.

#### 2.1.7 Inference Timing Instrumentation — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionRunner.cpp:210-223`

Wall-clock inference time is measured around `session->Run()` using `steady_clock` (monotonic). The last and maximum inference times are stored in relaxed atomics for diagnostic reads. A sustained value approaching the 3 s cycle budget would indicate the model can no longer keep up.

#### 2.1.8 Error Capture from ORT Exceptions — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionRunner.cpp:234-253`

ORT exceptions are caught, truncated to a 256-byte fixed buffer (no heap allocation from the analysis thread), and stored for later retrieval via `getLastRunError()`. Previously the exception was swallowed entirely, leaving only a `false` return.

---

### 2.2 What Is Broken, Incorrect, or Inefficient

#### 2.2.1 The Model Is Loudness-Blind (AUDIT-7) — DESIGN LIMITATION
**Severity:** High (semantic risk)  
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:581-587`, `AGENTS.md`

Because the input is peak-normalized to `-1 dBFS`, the model **cannot measure absolute input LUFS**. The decoded loudness slot is a **caller-supplied target**, not a measurement. The AGENTS.md explicitly documents this: "The model is loudness-blind: the input is peak-normalized to −1 dBFS before inference, so the model cannot measure absolute input LUFS."

**Root Cause:** The ONNX graph takes only one input (waveform). No loudness target is fed into the graph at inference time.  
**Impact:** The assistant cannot use the model's "loudness analysis" as a measurement. Genuine measurements must come from `SonicMasterMeasurementSnapshot` (BS.1770-4 LUFS meter).  
**Fix:** Not a code fix — a documentation/UX fix. The assistant must be trained to never present the model's loudness recommendation as a measurement.

#### 2.2.2 No Embedded Genre Classifier Model — FEATURE GAP
**Severity:** Medium  
**Evidence:** `AGENTS.md` (GenreClassifier section), `src/AI/GenreClassifier.h/cpp`

The `GenreClassifier` runs a **time-domain heuristic** (low/high band split at 200 Hz + zero-crossing rate) out of the box, producing a coarse genre guess with confidence 0.5–0.65. A neural genre model is **pluggable** but not embedded. The user must manually drop a compatible `genre_classifier.onnx` into `%APPDATA%/MorePhi/models/`.

**Impact:** Genre-conditioned priors (Stage 1 target LUFS, Stage 2 tonal-balance residual) are driven by a heuristic guess with low confidence. The system works "from day one" but with reduced accuracy.  
**Fix:** Ship an embedded genre model, or document the pluggable path clearly.

#### 2.2.3 Missing Audio Metrics in Analysis Snapshot — MEASUREMENT GAP
**Severity:** Medium  
**Evidence:** `AGENTS.md` (SonicMaster Audit Notes), `src/AI/SonicMasterAnalysisEngine.h:105-120`

The following metrics are **not computed**: noise floor, SNR, muddiness, harshness, low/high energy ratio, transient/attack/punch. The available metrics are: LUFS (I/S/M), LRA, true peak, spectral centroid, spectral tilt, stereo width, correlation, THD%, and crest factor.

**Impact:** The assistant has incomplete information for mastering decisions.  
**Fix:** Add the missing metric computations to `RealtimeSpectrumAnalyzer` or `AutoMasteringEngine::analyzeBlock`.

#### 2.2.4 Inference Window Is ~5.94 s (Not 6 s) — DOCUMENTATION DRIFT
**Severity:** Low  
**Evidence:** `src/AI/SonicMasterDecisionRunner.h:41`, `AGENTS.md`

`kSonicMasterSegmentFrames = 262138` at 44.1 kHz = 5.94 s, not 6 s. Some user-facing copy says "~6 s of captured audio." The 0.06 s discrepancy is negligible but should be corrected in user-facing docs.

#### 2.2.5 Mono Downmix Path Exists but Is Untested for Model Inference — RISK
**Severity:** Low  
**Evidence:** `AGENTS.md` (Mono capture supported)

The capture ring supports mono downmix (`channelCount == 1`), but the ONNX model expects stereo input `[1, 2, N]`. The mono path would need to duplicate the mono channel to stereo before inference. It is unclear if this is done. The `SonicMasterDecisionRunner` does not have a mono-to-stereo expansion step.

**Fix:** Verify and test the mono path, or document it as unsupported for neural inference.

---

## 3. Layer 2: Model-to-Plugin Wiring

### 3.1 What Is Working Correctly

#### 3.1.1 44-Float Decision Decoder Is Correct and Deterministic — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionDecoder.cpp`, `src/AI/SonicMasterDecisionDecoder.h`

The decoder maps the 44-float output vector to mastering parameters with the following layout (verified against `mastering_decision_adapter.py`):

| Offset | Count | Meaning |
|--------|-------|---------|
| 0 | 8 | EQ band gains (dB, clamped ±12) |
| 8 | 1 | Target LUFS (clamped [-23, -8]) |
| 9 | 1 | True-peak ceiling (clamped [-3, -0.1]) |
| 10 | 1 | Compressor gate |
| 11 | 18 | 3 bands × (threshold, ratio, attack, release, makeup, knee) |
| 29 | 1 | Exciter gate |
| 30 | 4 | 2 bands × (drive, mix) |
| 34 | 1 | Stereo gate |
| 35 | 4 | 2 regions × (width, sideGain) |
| 39 | 1 | Aggression |
| 40 | 1 | Gain reduction |
| 41 | 3 | Character logits (transparent/balanced/aggressive) |

All values are clamped to ranges the DSP modules can honor. Non-finite values are coerced to a fallback.

#### 3.1.2 Decoder and Engine Clamp Ratio to the SAME Range — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionDecoder.h:35-41`, `src/AI/SonicMasterDecisionDecoder.cpp:135`, `src/Core/AutoMasteringEngine.cpp:572`

Both the decoder and the engine clamp compressor ratio to `[kSonicMasterCompRatioMin, kSonicMasterCompRatioMax] = [1.0, 4.0]`. Previously the decoder allowed up to 6.0, which caused the normalized value `(ratio-2.5)/1.5` to exceed the safety policy's `[-1, +1]` bounds, leading to silent `TargetOutOfRange` rejections.

#### 3.1.3 LUFS Clamp to Engine Output Range — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionDecoder.cpp:73-95`

The decoder clamps the target LUFS to `[-23, -8]`, which matches the engine's actual output range. Previously a model target of `-6` LUFS would decode to `value=1.33` and the engine would silently clamp back to `-8`, losing the model's expressed extreme and lying to telemetry.

#### 3.1.4 Full Compressor Sidecar Preserved — VERIFIED
**Evidence:** `src/AI/SonicMasterDecisionDecoder.cpp:131-157`, `src/Core/NeuralMasteringTypes.h:33-41`

The decoder extracts all six compressor parameters per band (threshold, ratio, attack, release, makeup, knee) into `NeuralMasteringCompBand` sidecars. The safety policy carries these through (`candidate.compParams`), and `applyValidatedPlan` applies them directly when `hasCompParams` is true. Previously only threshold and ratio were preserved, and attack/release/makeup/knee were dropped.

#### 3.1.5 Capture Ring Is Eagerly Allocated — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:300-321`, `TestSonicMasterAnalysisEngine.cpp:247-271`

The capture ring is allocated **eagerly in `prepare()`** rather than lazily on first `setActive(true)`. This fixes the on-demand path (`requestDecisionNow` / `sonicmaster_decision`) which previously failed with "no fresh audio captured" because the ring was null during playback. The ring is rate-proportional: ~4 MiB at 48 kHz, ~16 MiB at 192 kHz.

#### 3.1.6 Transition Guard Discards Contaminated Windows — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:535-567`, `AGENTS.md` (P2.8)

When a hosted-plugin parameter changes, `notifyHostedParameterChanged()` arms a flag. The next analysis cycle checks if the capture window straddles the change. If so, the window is discarded, the ring is flushed, and the cycle reports `CollectingAudio` until a clean post-settle window accumulates. The default settle time is 0.5 s (configurable).

#### 3.1.7 ONNX→HTTP Failover on Consecutive Failures — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:622-626`, `AGENTS.md` (AUDIT LOW-4)

If the primary ONNX source fails `consecutiveFailureLimit` (default 3) times, the engine automatically swaps to a fallback HTTP source. Only if the fallback also fails does the engine auto-disable (`ErrorAutoDisabled`).

#### 3.1.8 Plan Staleness Guard — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:930-941`, `src/AI/SonicMasterAnalysisEngine.cpp:525-526`

Each plan is stamped with the capture window's steady-clock instant (`captureTimeNs_`). In `applyRamped()`, if the plan is older than 10 s, it is discarded rather than applied against audio it no longer describes. This prevents stale plans from being applied after long pauses or DAW transport stops.

#### 3.1.9 Closed-Loop LUFS Feedback (Stage D) — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:729-775`, `src/AI/SonicMasterAnalysisEngine.h:287-303`

After each apply, the engine measures the achieved LUFS on the captured (post-hosted-plugin) signal and folds a bounded correction into the next cycle's target:
- Deadband: 0.5 LU (no correction if within 0.5 LU of target)
- Max correction per cycle: 1.0 LU
- Target clamp: [-23, -8] LU

This prevents runaway oscillation. The loop is gated on finiteness (`std::isfinite(m.lufsIntegrated)`) to avoid `+inf` error when the meter hasn't crossed its gate yet (fixed F3.2).

#### 3.1.10 Genre Priors (Stage 1 & Stage 2) — VERIFIED
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:647-650`, `src/AI/SonicMasterDecisionDecoder.cpp:45-61`, `AGENTS.md`

- **Stage 1:** `GenreMasteringProfile` maps each genre to a target LUFS. The analysis engine pushes this into `setGenreTargetLufs()`.
- **Stage 2:** `TonalBalanceExtractor` computes the 8-band measured tonal balance from the live spectrum. The decoder blends `clamp(target - measured, ±6 dB) * residualBlend` into each EQ band before the ±12 dB clamp. The blend is scaled by `getTopConfidence()` (capped at 0.5×) and only applied when confidence ≥ 0.5.

Decode precedence: **closed-loop feedback > genre prior > model default**.

---

### 3.2 What Is Broken, Incorrect, or Inefficient

#### 3.2.1 Internal DSP Chain Is DORMANT in Shipped Plugin — CRITICAL DESIGN CHOICE
**Severity:** Critical (for standalone operation)  
**Evidence:** `src/Plugin/PluginProcessor.cpp:1668`, `src/Core/AutoMasteringEngine.cpp:538-684`, `AGENTS.md`

The shipped plugin calls `autoMasteringEngine_.prepare(sampleRate, samplesPerBlock, **false**)` — `startIntelligence = false`. This means:
- `intelligenceActive_ = false`
- All internal DSP objects (`eq_`, `dynamics_`, `stereo_`, `exciter_`, `limiter_`, `normalizer_`) are dormant
- `applyValidatedPlan` skips the internal-chain writes and logs once: "internal DSP chain dormant; forwarding plan to hosted plugin only."

**Root Cause:** The internal chain is not activated in the shipped build. This is intentional but means the plugin **cannot master audio without a hosted plugin**.  
**Impact:** Users expecting a self-contained mastering solution will get silence (no processing). The neural model's decisions are silently forwarded to the Ozone bridge.  
**Fix:** Change `prepare(..., true)` in `PluginProcessor.cpp` or add a user-facing toggle that activates the internal chain. If the internal chain is to remain dormant, document this prominently.

#### 3.2.2 Ozone Parameter Map Requires Manual Audit — CRITICAL OPERATIONAL GAP
**Severity:** Critical (for hosted-plugin path)  
**Evidence:** `src/AI/OzonePlanApplicator.cpp:75-93`, `AGENTS.md` (AUDIT-FIX-5)

If the Ozone parameter map has never been audited (`audit_ozone_parameters` not run), `map_.hasAnyMapping()` returns `false`, and `OzonePlanApplicator::apply()` returns 0 with a `DBG` warning. The neural plan reaches **zero** hosted plugin parameters.

```cpp
if (!map_.hasAnyMapping())
{
    DBG("OzonePlanApplicator: WARNING — parameter map is all-stubs; "
        "no Ozone parameters will be set. Run audit_ozone_parameters(apply=true) "
        "against the hosted plugin to populate the map.");
    lastAppliedCount_ = 0;
    return 0;
}
```

**Root Cause:** The map is built lazily and is empty by default.  
**Impact:** Every neural apply is a silent no-op until the user runs the audit tool.  
**Fix:** Auto-run the audit on plugin load, or at least surface a prominent UI/MCP warning that the map is unpopulated.

#### 3.2.3 No Impact Slot in 44-Float Decision Vector — FEATURE GAP
**Severity:** Medium  
**Evidence:** `src/AI/SonicMasterDecisionDecoder.h`, `src/Core/AutoMasteringEngine.cpp:609-622`

The transient shaper (`TransientShaper`) exists in the internal chain but the 44-float decision vector has **no impact slot**. When the mask is raised (not by default), a fixed amount of `0.4f` is applied.

```cpp
if (plan.appliedMask.impact)
{
    transient_.setEnabled(true);
    transient_.setAmount(0.4f);   // gentle transient lift
}
```

**Impact:** The transient shaper cannot be modulated by the neural model.  
**Fix:** Add an impact slot to the model export and decode it in `decodeSonicMasterDecision`.

#### 3.2.4 Limiter Mask Is OFF by Default — SAFETY POSTURE
**Severity:** Medium (design choice)  
**Evidence:** `src/AI/SonicMasterDecisionDecoder.cpp:97-113`

The decoder leaves `appliedMask.limiter = false`. The true-peak ceiling is decoded for telemetry but **not applied by default**. The safety policy treats the limiter as high-risk. A streaming-safe ceiling clamp (`kStreamingSafeCeilingDBTP`, typically `-1.0 dBTP`) is applied regardless, but the limiter itself is not engaged.

**Impact:** Without the limiter active, a loud plan with high makeup gain could clip before the normalizer catches it. The streaming-safe clamp mitigates but does not provide brickwall protection.  
**Fix:** If the internal chain is activated, consider defaulting the limiter mask ON with a conservative ceiling.

#### 3.2.5 Exciter/Harmonic Mask Is OFF by Default — SAFETY POSTURE
**Severity:** Low  
**Evidence:** `src/AI/SonicMasterDecisionDecoder.cpp:170-173`

The decoder decodes saturation/exciter values but leaves `appliedMask.harmonic = false`. This matches the "DeterministicBaseline" safety posture.

**Impact:** Harmonic enhancement is not applied by default.  
**Fix:** If desired, add an opt-in toggle for the assistant to raise the harmonic mask.

---

## 4. Layer 3: AI Assistant Integration

### 4.1 What Is Working Correctly

#### 4.1.1 Structured Failure Reasons for the Assistant — VERIFIED
**Evidence:** `src/AI/MCPToolHandler.cpp:101-114`, `src/AI/SonicMasterAnalysisEngine.h:141-150`, `src/AI/SonicMasterAnalysisEngine.cpp:780-918`

The `sonicmaster_decision` MCP tool now returns **structured failure reasons** instead of a generic opaque string. The `DecisionFailure` enum distinguishes:
- `NotPrepared` — model not loaded
- `InsufficientAudio` — ring not full yet
- `SilentInput` — captured silence
- `InferenceRejected` — ONNX Run threw
- `DecodeFailed` — output shape/contract mismatch
- `SafetyRejected` — plan failed safety policy

This prevents the assistant from confabulating causes (e.g., "inference server down" when the real issue was "not enough audio captured").

#### 4.1.2 Safety Rejection Detail with Issue Classification — VERIFIED
**Evidence:** `src/AI/MCPToolHandler.cpp:253-319`, `src/AI/SonicMasterAnalysisEngine.cpp:215-249`

When the safety policy rejects a plan, the MCP tool reports:
- `primary_issue` (e.g., `low_confidence`, `target_out_of_range`, `non_finite_value`)
- `hard_reject` flag (retrying same audio won't help)
- `candidate_confidence` (e.g., "model confidence was 0.4, floor is 0.75")
- Full `issues` array

The assistant can now give honest, actionable next steps instead of generic "try again" advice.

#### 4.1.3 Live Measurements JSON with Finiteness Guards — VERIFIED
**Evidence:** `src/AI/MCPToolHandler.cpp:147-187`, `src/AI/SonicMasterAnalysisEngine.cpp:997-1035`

The `SonicMasterMeasurementSnapshot` pulls genuine BS.1770-4 measurements from the live meters. The JSON serialization:
- Guards against `-inf` LUFS (previously serialized as `null`) using `finiteOr()`
- Adds a `measured_finite` boolean flag so the assistant can distinguish "engine attached, no signal yet" from "measurement failed"
- Explicitly labels the source as `genuine_input_measurement_NOT_model_estimate`

#### 4.1.4 Closed-Loop JSON for Assistant Telemetry — VERIFIED
**Evidence:** `src/AI/MCPToolHandler.cpp:194-204`

The `sonicmasterClosedLoopJson` helper reports:
- `feedback_active` — is the loop correcting?
- `last_applied_target_lufs` — what was targeted
- `last_measured_lufs` — what was achieved
- `last_lufs_error` — positive = too quiet
- `next_target_lufs` — what the next cycle will target

This gives the assistant visibility into the convergence behavior.

#### 4.1.5 Heuristic Fallback for On-Demand Calls — VERIFIED
**Evidence:** `src/AI/MCPToolHandler.cpp:215-240`

If the neural path fails, the assistant can fall back to the rule-based heuristic via `buildRuleBasedInputFromLiveMeters()`. This fallback uses the **measured** LUFS and true-peak (which the neural model cannot see) and can handle already-hot material that the neural model refuses.

#### 4.1.6 Action Ledger Records Neural Writes — VERIFIED
**Evidence:** `src/Core/AutoMasteringEngine.cpp:754-777`, `AGENTS.md` (P2.5)

Neural mastering applies are recorded in the ActionLedger with:
- Tool name: `neural_mastering.apply_validated_plan`
- Risk: `HighImpact`
- Ozone enqueued count, verification breakdown, partial flag

This closes the audit gap where neural writes bypassed `MCPToolHandler::handle` and left no trace.

#### 4.1.7 Agent Runtime Isolation from Audio Thread — VERIFIED
**Evidence:** `AGENTS.md` (Threading invariant), `src/AI/Agents/Scheduler/PriorityScheduler.h`

The multi-agent orchestration layer executes **only on scheduler worker threads** (2 threads). `RealtimeControlAgent` writes corrections through `LockFreeQueue`, exactly like the UI/MCP paths. Agents never run on the audio thread.

#### 4.1.8 LLM Transport Selection at Runtime — VERIFIED
**Evidence:** `AGENTS.md` (ConductorAgent row, LLM section)

`MorePhiProcessor::startMCPServerIfNeeded()` constructs:
- `RestLlmClient` when a provider API key is configured (non-empty key + selected model)
- `DeterministicFallbackLlmClient` (heuristic) otherwise

The selection gates on **configured**, not validated. A configured-but-invalid key wires `RestLlmClient` and fails lazily to `http_401` on the first `complete()`, which is handled with retry. This avoids stalling MCP server init on a network call.

---

### 4.2 What Is Broken, Incorrect, or Inefficient

#### 4.2.1 Readback Verification Runs BEFORE Audio Thread Drains — TIMING ARTIFACT
**Severity:** Medium  
**Evidence:** `src/Core/AutoMasteringEngine.cpp:708-745`, `src/AI/OzonePlanApplicator.cpp:24-67`

The `getLastVerification()` readback runs immediately after `applyPlan()`, **before** the audio thread drains the command queue. Writes that have been enqueued but not yet drained read back as their pre-write values and are counted as `mismatched`.

**Root Cause:** The readback is synchronous on the message thread; the audio thread hasn't processed the queue yet.  
**Impact:** A clean apply can spuriously show `mismatched` counts. The `confirmedFraction()` fix (AUDIT-F2.3) excludes `mismatched` from the partial classifier, so this is now a cosmetic issue rather than a functional one.  
**Fix:** Add a post-drain callback that re-runs verification after the audio thread has processed the queue. Or, document that `mismatched` is expected for recently-enqueued params.

#### 4.2.2 Assistant Cannot Distinguish "Applied" from "AppliedNoAudioPath" Without Polling — UX GAP
**Severity:** Low  
**Evidence:** `src/AI/SonicMasterAnalysisEngine.cpp:715-720`

The engine distinguishes `Applied` (reached audio) from `AppliedNoAudioPath` (internal chain dormant, Ozone wrote 0 params) via the `Status` enum. However, the MCP tool's success JSON does not explicitly echo this distinction unless the caller reads `engine_status`.

**Fix:** Add `reached_audio_path: true/false` to the `sonicmaster_decision` success JSON.

#### 4.2.3 No Streaming-Safe Ceiling Applied to Hosted Plugin Unless Explicitly Requested — RISK
**Severity:** Medium  
**Evidence:** `src/AI/OzonePlanApplicator.cpp:238-248`, `src/Core/AutoMasteringEngine.cpp:641-659`

The streaming-safe ceiling clamp (`kStreamingSafeCeilingDBTP`) is applied to the **internal** limiter in `applyValidatedPlan`, but the Ozone bridge applies whatever ceiling the plan contains. The `MultiEffectPlan` ceiling is clamped to `[-3, -0.1]` in `buildBridgePlanFromNeural`, but there is no explicit `-1.0 dBTP` streaming-safe cap for the Ozone maximizer unless the plan's `applyLimiterCeiling` is set.

**Fix:** Hard-cap the Ozone maximizer ceiling to `kStreamingSafeCeilingDBTP` in `buildBridgePlanFromNeural` regardless of the limiter mask state.

---

## 5. Layer 4: Mastering Chain Application

### 5.1 What Is Working Correctly

#### 5.1.1 Per-Cycle Delta Cap Enforcement at Apply Time — VERIFIED
**Evidence:** `src/Core/AutoMasteringEngine.cpp:525-528`, `src/Core/NeuralMasteringSafetyPolicy.h:71-73`

`applyGuardPolicy_.enforceDeltaCaps()` clamps the plan's `projectedTargets` against the last safe baseline. This closes the gap where a direct in-process caller (test, future MCP tool) could bypass the 0.6 LU/cycle loudness slew limit. The clamp count is stored in `lastApplyDeltaClamps_` for telemetry.

#### 5.1.2 Streaming-Safe Ceiling on Internal Limiter — VERIFIED
**Evidence:** `src/Core/AutoMasteringEngine.cpp:651-659`

After every neural apply, the internal limiter ceiling is clamped to `kStreamingSafeCeilingDBTP` (typically `-1.0 dBTP`). This is the Spotify/YouTube/Apple streaming standard. The post-clamp ceiling is observable via `getLastAppliedCeilingDbtp()`.

#### 5.1.3 EQ Application Limited to 8 Model Bands — VERIFIED
**Evidence:** `src/Core/AutoMasteringEngine.cpp:540-551`

`applyValidatedPlan` only writes the first 8 EQ bands (the model's decision count). Bands 8–31 remain untouched, preserving the genre translator's warm-start character. Previously the loop ran to 32 and force-wrote bands 8–31 to 0 dB, wiping the genre character.

#### 5.1.4 Dynamics Application Limited to 3 Model Bands — VERIFIED
**Evidence:** `src/Core/AutoMasteringEngine.cpp:554-587`

Only the 3 compressor bands from the model are applied. Band 3 (High) stays on the heuristic warm-start. When `hasCompParams` is true, all six real-unit parameters are applied directly.

#### 5.1.5 Stereo Width Application Limited to 2 Model Regions — VERIFIED
**Evidence:** `src/Core/AutoMasteringEngine.cpp:590-598`

Only 2 width regions are applied. Regions 2–3 (Mid/High) stay on the genre translator / mono-checker callback.

#### 5.1.6 Heuristic Timer Defers to Recent Neural Plan — VERIFIED
**Evidence:** `src/Core/AutoMasteringEngine.cpp:458-463`

The 30-second heuristic timer (`ChainPlanExecutor`) now defers loudness, stereo width, and EQ to a recent neural plan when one exists. This prevents the heuristic from clobbering the neural path's decisions.

#### 5.1.7 Discrete Parameter Snap Before Enqueue — VERIFIED
**Evidence:** `src/AI/OzonePlanApplicator.cpp:301`

`enqueueIfMapped` snaps discrete/binary parameters to the nearest valid step via `snapNormalizedToStep()` before enqueueing. This prevents the hosted plugin from receiving unreliable continuous values for discrete controls.

#### 5.1.8 Index-Drift Re-Validation — VERIFIED
**Evidence:** `src/AI/OzonePlanApplicator.cpp:282-295`, `src/AI/OzonePlanApplicator.cpp:348-393`

Before writing to a parameter index, the applicator re-reads the hosted plugin's current parameter name at that index and compares it to the expected name using a tokenized subset matcher. If the plugin was swapped after the map was built, the write is skipped rather than corrupting an unrelated control.

#### 5.1.9 Neural Writes Hold Against Morph — VERIFIED
**Evidence:** `src/AI/OzonePlanApplicator.cpp:302-318`

Neural plan writes are tagged with `ParameterEditSource::Neural` and `holdAgainstMorph = true`. This prevents a running morph from overwriting the neural recommendation before the command queue drains. The MCP path already had this invariant; the neural path now matches it.

---

### 5.2 What Is Broken, Incorrect, or Inefficient

#### 5.2.1 Apply Is Instant (No Ramps) — CLICK RISK
**Severity:** Medium (if internal chain activated)  
**Evidence:** `src/Core/AutoMasteringEngine.cpp:511-513`

The code comment explicitly states: "instant parameter set is fine because the internal chain is dormant in the shipped plugin. If activated, add 5-10ms ramps to avoid discontinuities on live audio."

**Root Cause:** No ramping is implemented.  
**Impact:** If the internal chain is ever activated, parameter changes will cause audible clicks. The Ozone bridge path already has ParameterBridge smoothing, so this only affects the internal chain.  
**Fix:** Implement 5-10 ms ramps in `eq_.setBandGain`, `dynamics_.setBandParams`, `stereo_.setWidth`, etc., gated on `intelligenceActive_`.

#### 5.2.2 No Plan Atomicity Across Audio Blocks — KNOWN LIMITATION
**Severity:** Low  
**Evidence:** `AGENTS.md` (P3.10), `src/AI/OzonePlanApplicator.cpp:104-113`

A plan boundary marker (`enqueuePlanBoundary`) is emitted after the last parameter, but the audio thread processes parameters block-by-block. There is no cross-block all-or-nothing buffering. The marker is an observability signal, not a transactional guarantee.

**Impact:** If a parameter is read mid-drain, the plugin state may reflect a partially-applied plan. In practice, this is a single-block window (~1-10 ms) and is negligible for mastering parameters.  
**Fix:** Document as a known limitation. True cross-block atomicity would require a command-queue redesign.

---

## 6. Layer 5: End-to-End Signal Flow

### 6.1 Verified Signal Path

**Audio Thread:**
```
processBlock() → analyzeBlock() (throttled, every N blocks)
              → sonicMasterEngine_.capture(left, right, n) (lock-free ring write)
```

**Analysis Thread (background, ~every 3 s):**
```
runCycle() → readNewest(hostFrames) from ring
          → resamplePolyphase(hostRate → 44.1k)
          → peakNormalize(-1 dBFS)
          → source_->infer() (ONNX or HTTP)
          → decodeSonicMasterDecision() (44 floats → plan)
          → safetyPolicy_.validate() (delta caps, confidence floor)
          → applyRamped() → pendingPlan_ + pendingApplication_.store(true)
```

**Message Thread (timer, ~every 50 ms):**
```
timerCallback() → if hasPendingApplication() → processPendingApplication()
               → AutoMasteringEngine::applyValidatedPlan()
               → [internal chain writes] (dormant in shipped plugin)
               → buildBridgePlanFromNeural() → MultiEffectPlan
               → chainPlanner_.applyPlan() → OzonePlanApplicator::apply()
               → enqueueParameterSet() → LockFreeQueue → audio thread drain
```

**Audio Thread (drain):**
```
processBlock() → drain LockFreeQueue commands → ParameterBridge → hosted plugin
```

### 6.2 End-to-End Test Evidence

#### 6.2.1 CAPTURE-DECOUPLE Regression Test — PASSED
**Evidence:** `tests/Unit/TestSonicMasterAnalysisEngine.cpp:247-271`

With `setActive(false)` (preview OFF), after `prepare()` the ring is eagerly allocated. Feeding silence for >6 s and calling `requestDecisionNow` succeeds. This was the exact production failure before the fix.

#### 6.2.2 Auto-Disable on Consecutive Failures — PASSED
**Evidence:** `tests/Unit/TestSonicMasterAnalysisEngine.cpp:273-295`

After 3 consecutive inference failures, the engine transitions to `ErrorAutoDisabled` and sets `active_ = false`. Auto-recovery is verified when the source heals.

#### 6.2.3 Safety Projection Applied, Not Raw Target — PASSED
**Evidence:** `tests/Unit/TestSonicMasterAnalysisEngine.cpp:147-175`

A source that emits `-8.0 LUFS` and `+12 dB EQ` is decoded, safety-projected (delta-capped), and applied. The applied EQ is `0.15` (12 dB × 0.15 = ~1.8 dB, within the delta cap), not the raw `+12 dB`.

#### 6.2.4 On-Demand Decision Does NOT Advance Safety Baseline — PASSED
**Evidence:** `tests/Unit/TestSonicMasterAnalysisEngine.cpp:177-209`

Two consecutive `requestDecisionNow` calls with the same aggressive source produce identical projected targets. The on-demand path does not mutate `lastSafeNeuralPlan_`, so exploratory decisions do not affect future delta-cap calculations.

#### 6.2.5 Insufficient Audio Skip — PASSED
**Evidence:** `tests/Unit/TestSonicMasterAnalysisEngine.cpp:211-232`

Feeding only 1000 frames (far short of the ~6 s window) causes `runCycle()` to return `false` with `Status::CollectingAudio` and `source.callCount() == 0` (no inference attempted).

---

### 6.3 End-to-End Issues

#### 6.3.1 No Audible Processing Without Hosted Plugin or Internal Chain Activation — CRITICAL
**Severity:** Critical  
**Evidence:** Entire signal flow analysis above.

If the user loads the plugin with:
1. No hosted plugin loaded, AND
2. Internal chain dormant (`intelligenceActive_ = false`)

Then the neural mastering system produces **no audible effect**. The analysis thread runs, inference succeeds, the plan is decoded and validated, but:
- The internal chain is dormant → no DSP processing
- No hosted plugin → Ozone applicator writes 0 parameters

**Root Cause:** The plugin is designed as a **hosting platform**, not a standalone mastering processor, in the shipped build.  
**Impact:** Users may enable "SonicMaster Analysis" and hear no change.  
**Fix:** Either activate the internal chain by default, or add a clear UI state: "SonicMaster ready — load a mastering plugin to apply decisions."

#### 6.3.2 Latency Reporting Does Not Include Analysis Thread Inference Time — DOCUMENTATION GAP
**Severity:** Low  
**Evidence:** `src/Plugin/PluginProcessor.cpp:4456`

`latencyManager_.setMasteringChainLatency()` reports the internal chain's lookahead latency (brickwall limiter + exciter oversampling). It does **not** include the ~3 s analysis cycle or the inference latency. The mastering decisions are applied asynchronously, so the user-facing latency is not affected by inference time, but the **decision delay** (time from audio change to parameter update) is ~3–6 s.

**Fix:** Document the decision delay in the user manual and MCP tool descriptions.

---

## 7. Priority-Ordered Recommendations

### P0 — Critical (Fix Before Production Release)

1. **Activate Internal DSP Chain or Warn Users**  
   The shipped plugin runs with `intelligenceActive_ = false`, meaning the neural model's decisions are inaudible without a hosted plugin. Either:
   - Change `PluginProcessor.cpp:1668` to `prepare(..., true)`, OR
   - Add a UI/MCP warning when `AppliedNoAudioPath` is detected: "Neural mastering is analyzing but not applying. Load a mastering plugin or enable the internal chain."

2. **Auto-Populate Ozone Parameter Map**  
   The Ozone applicator silently applies 0 parameters if `audit_ozone_parameters` has never run. Implement auto-audit on plugin load or at least a blocking warning: "Parameter map is empty — run audit_ozone_parameters to enable neural mastering."

### P1 — High (Fix Soon)

3. **Add Streaming-Safe Ceiling Cap to Ozone Bridge**  
   Hard-cap the Ozone maximizer ceiling to `kStreamingSafeCeilingDBTP` in `buildBridgePlanFromNeural`, regardless of the limiter mask state. Currently the internal limiter is capped but the Ozone bridge is not.

4. **Implement Parameter Ramps for Internal Chain**  
   If the internal chain is activated, add 5–10 ms ramps to `eq_.setBandGain`, `dynamics_.setBandParams`, `stereo_.setWidth`, and `limiter_.setCeiling` to avoid clicks. The comment at `AutoMasteringEngine.cpp:511` acknowledges this gap.

5. **Add Missing Audio Metrics**  
   Compute noise floor, SNR, muddiness, harshness, low/high energy ratio, and transient/attack/punch in `analyzeBlock()` and expose them in `SonicMasterMeasurementSnapshot`.

6. **Ship or Document Genre Classifier Model**  
   Either embed a `genre_classifier.onnx` or add a prominent UI hint: "Genre classification is running a heuristic. Drop a compatible ONNX model into `%APPDATA%/MorePhi/models/` for neural genre detection."

### P2 — Medium (Fix When Convenient)

7. **Add Impact Slot to 44-Float Decision Vector**  
   Export a transient-shaper amount from the model and decode it in `decodeSonicMasterDecision`. Currently the mask is binary and the amount is fixed at 0.4.

8. **Post-Drain Verification Re-Read**  
   Add a callback after the audio thread drains the command queue to re-run `getLastVerification()` and convert timing-artifact `mismatched` counts into genuine `verified` counts.

9. **Mono-to-Stereo Expansion for Model Input**  
   Verify that mono capture is correctly duplicated to stereo before ONNX inference. If not, add the expansion in `SonicMasterDecisionRunner::runDecision`.

10. **Document Decision Delay**  
    The ~3 s analysis cycle + inference time means mastering decisions lag audio changes by 3–6 s. Document this clearly for users and in the MCP tool descriptions.

### P3 — Low (Nice to Have)

11. **Correct "~6 s" to "~5.94 s" in User-Facing Copy**  
    `kSonicMasterSegmentFrames = 262138` at 44.1 kHz = 5.94 s. Update docs and MCP tool descriptions.

12. **Add `reached_audio_path` to `sonicmaster_decision` Success JSON**  
    Make the `Applied` vs `AppliedNoAudioPath` distinction explicit in the tool response.

13. **Add True Cross-Block Plan Atomicity**  
    Implement a double-buffered command queue so the audio thread swaps entire plans atomically rather than draining parameters one by one.

---

## 8. Appendix: Verified Working Behaviors (Quick Reference)

| Behavior | Evidence | Status |
|----------|----------|--------|
| ONNX input de-interleave into [1,2,N] | `SonicMasterDecisionRunner.cpp:188-194` | ✅ Fixed |
| Contract .json validation at load | `SonicMasterDecisionRunner.cpp:49-64` | ✅ Fixed |
| Rank + shape validation | `SonicMasterDecisionRunner.cpp:121-135` | ✅ Fixed |
| Ratio clamp aligned [1,4] | `SonicMasterDecisionDecoder.h:35-41` | ✅ Fixed |
| LUFS clamp to engine range [-23,-8] | `SonicMasterDecisionDecoder.cpp:73-95` | ✅ Fixed |
| Full compressor sidecar (6 params/band) | `SonicMasterDecisionDecoder.cpp:131-157` | ✅ Fixed |
| Eager capture ring allocation | `SonicMasterAnalysisEngine.cpp:300-321` | ✅ Fixed |
| Transition guard (P2.8) | `SonicMasterAnalysisEngine.cpp:535-567` | ✅ Fixed |
| Closed-loop LUFS feedback (Stage D) | `SonicMasterAnalysisEngine.cpp:729-775` | ✅ Fixed |
| Structured failure reasons | `MCPToolHandler.cpp:101-114` | ✅ Fixed |
| Safety rejection detail | `MCPToolHandler.cpp:253-319` | ✅ Fixed |
| Live measurements with finiteness guards | `MCPToolHandler.cpp:147-187` | ✅ Fixed |
| Plan staleness guard | `SonicMasterAnalysisEngine.cpp:930-941` | ✅ Fixed |
| Delta cap at apply time | `AutoMasteringEngine.cpp:525-528` | ✅ Fixed |
| Streaming-safe ceiling | `AutoMasteringEngine.cpp:651-659` | ✅ Fixed |
| EQ limited to 8 model bands | `AutoMasteringEngine.cpp:540-551` | ✅ Fixed |
| Dynamics limited to 3 model bands | `AutoMasteringEngine.cpp:554-587` | ✅ Fixed |
| Stereo limited to 2 model regions | `AutoMasteringEngine.cpp:590-598` | ✅ Fixed |
| Heuristic defers to neural plan | `AutoMasteringEngine.cpp:458-463` | ✅ Fixed |
| Discrete snap before enqueue | `OzonePlanApplicator.cpp:301` | ✅ Fixed |
| Index-drift re-validation | `OzonePlanApplicator.cpp:282-295` | ✅ Fixed |
| Neural writes hold against morph | `OzonePlanApplicator.cpp:302-318` | ✅ Fixed |
| Action ledger for neural writes | `AutoMasteringEngine.cpp:754-777` | ✅ Fixed |
| Genre priors (Stage 1 & 2) | `SonicMasterAnalysisEngine.cpp:647-650` | ✅ Fixed |
| ONNX→HTTP failover | `SonicMasterAnalysisEngine.cpp:622-626` | ✅ Fixed |
| Inference timing instrumentation | `SonicMasterDecisionRunner.cpp:210-223` | ✅ Fixed |
| ORT error capture | `SonicMasterDecisionRunner.cpp:234-253` | ✅ Fixed |
| Agent thread isolation | `AGENTS.md` (Threading invariant) | ✅ Fixed |
| LLM transport selection | `AGENTS.md` (ConductorAgent row) | ✅ Fixed |

---

*End of Audit Report*
