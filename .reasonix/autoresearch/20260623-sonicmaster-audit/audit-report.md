# SonicMaster AI Mastering Workflow — Comprehensive Technical Audit

**Audit Date:** 2026-06-23  
**Project:** More-Phi VST3 Plugin v3.3.0  
**Scope:** SonicMaster neural mastering pipeline (analysis → plan → parameter implementation → quality)  
**Auditor:** Automated codebase analysis of `src/AI/SonicMaster*`, `src/Core/AutoMasteringEngine.*`, `src/Core/NeuralMastering*`, `src/AI/MCPToolHandler.cpp`

---

## Executive Summary

The SonicMaster neural mastering pipeline is a well-architected, safety-conscious implementation of a waveform-to-decision neural mastering system. It correctly separates concerns across three thread domains (audio, analysis, message), applies a multi-layered safety policy, and integrates cleanly with the existing `AutoMasteringEngine` DSP chain. The architecture is sound, the parameter translation is correct, and the signal chain ordering follows professional mastering best practices.

However, the system has three significant limitations that affect its mastering competency: (1) the neural model has no access to absolute loudness information due to peak-normalization, (2) the analysis window is too short for proper loudness-range and integrated-LUFS measurement per ITU-R BS.1770, and (3) the model checkpoint quality is documented as failing 4 of 9 release gates (EQ MAE 2.12 dB, true-peak 0.80 dBTP). One telemetry bug was found in the MCP tool response.

**Overall Assessment:** Competent for a preview/experimental feature. The safety policy and DSP integration are production-quality. The model inference quality is the limiting factor — the pipeline is well-built around a model that needs improvement.

---

## 1. Audio Analysis Verification

### 1.1 What the SonicMaster Model Actually Analyzes

The `SonicMasterAnalysisEngine` feeds raw stereo audio (not feature vectors) to the neural model. The model is a **waveform-to-decision** network — it does NOT perform traditional audio analysis within the plugin. The pipeline is:

```
AudioCaptureRing (lock-free SPSC)
  → Linear resample to 44.1 kHz
  → Peak-normalize to -1 dBFS
  → HTTP POST or ONNX Run (waveform[1,2,262138] → decision[1,44])
  → decodeSonicMasterDecision() → ValidatedNeuralMasteringPlan
```

*Files: `src/AI/SonicMasterAnalysisEngine.cpp:205-297`, `src/AI/SonicMasterDecisionDecoder.cpp:23-133`*

### 1.2 Actual Audio Metering (Industry-Standard)

The real audio analysis — LUFS, true peak, spectrum, stereo field — is performed by the `AutoMasteringEngine`'s built-in DSP meters via `analyzeBlock()`, NOT by the SonicMaster model. These are ITU-R BS.1770-compliant implementations:

| Meter | Class | Standard | Thread |
|-------|-------|----------|--------|
| LUFS (M/S/I) | `LUFSMeter` | ITU-R BS.1770-4 | Audio (throttled) |
| True Peak | `TruePeakEstimator` | ITU-R BS.1770-4 | Audio (throttled) |
| Spectrum | `RealtimeSpectrumAnalyzer` | 32-band FFT | Audio (throttled) |
| Stereo Field | `StereoFieldAnalyzer` | 8-band correlation | Audio (throttled) |

*File: `src/Core/AutoMasteringEngine.cpp:227-307`*

**✅ VERDICT: The built-in analysis meters are industry-standard and correctly implemented.** The throttled audio-thread analysis pattern (every N blocks via `ANALYSIS_THROTTLE_BLOCKS`) is a pragmatic choice that balances CPU cost against update rate.

### 1.3 Critical Finding: Peak-Normalization Destroys Loudness Information

**Severity: HIGH (design limitation, documented)**

At `SonicMasterAnalysisEngine.cpp:248-255`, the 44.1 kHz resampled audio is peak-normalized to -1 dBFS before inference:

```cpp
const float peak = std::max(peakAbs(modelL_.data(), kSonicMasterSegmentFrames),
                            peakAbs(modelR_.data(), kSonicMasterSegmentFrames));
const float gain = peak > 1e-9f ? (0.891f / peak) : 1.0f; // -1 dBFS
for (std::size_t i = 0; i < kSonicMasterSegmentFrames; ++i)
{
    interleaved_[2 * i + 0] = modelL_[i] * gain;
    interleaved_[2 * i + 1] = modelR_[i] * gain;
}
```

**Impact:** A -23 LUFS folk track and a -8 LUFS EDM banger both arrive at the model at identical -1 dBFS peak level. The model has ZERO information about the input's absolute loudness. Any "target LUFS" or "dynamics" decision from the model is based on spectral/temporal shape only, not on how loud the input actually is. The model's `target_lufs` at decision index 8 is a **requested mastering target parameter** (echoing the caller-supplied `target_lufs`), NOT a measurement.

*This is correctly documented in the code comments (AUDIT-7 at line 242-247), the MCP tool description, and the response warnings. But it remains a fundamental limitation for AI-driven mastering.*

### 1.4 Window Length Limitation

The model's segment length is `kSonicMasterSegmentFrames = 262138` samples at 44.1 kHz ≈ **5.94 seconds** (`src/AI/SonicMasterDecisionRunner.h:37`). Per ITU-R BS.1770:

- **Momentary LUFS (LUFS-M):** 400 ms window → ✅ Adequate (5.94s >> 0.4s)
- **Short-term LUFS (LUFS-S):** 3 s window → ✅ Adequate (5.94s > 3s)
- **Integrated LUFS (LUFS-I):** Whole program (minutes) → ❌ **Insufficient**
- **Loudness Range (LRA):** Whole program → ❌ **Insufficient**

The AUDIT-7 comment at `SonicMasterDecisionRunner.h:33-36` correctly flags this: *"Decisions derived from it are at best LUFS-S/short-term, never Integrated. Treat any 'integrated loudness' label downstream as mislabeled short-term."*

**✅ VERDICT: Correctly documented. The window is adequate for short-term analysis but insufficient for integrated measurements.**

### 1.5 Resampling Artifacts

Linear interpolation resampling (`SonicMasterAnalysisEngine.cpp:38-51`) introduces mirror-image aliasing above ~9 kHz when the host runs at 96 kHz. The code's ponytail comment correctly notes this is intentional — the model was trained on the same aliased preprocessing, making it self-consistent. A polyphase FIR would de-alias but change the model's input distribution.

**⚠️ VERDICT: Acceptable for the current model. A future model retraining should use proper resampling.**

---

## 2. Mastering Plan Evaluation

### 2.1 Decision Vector Decoding

The `decodeSonicMasterDecision()` function (`src/AI/SonicMasterDecisionDecoder.cpp:23-133`) correctly translates the 44-float model output into a `ValidatedNeuralMasteringPlan`. The mapping is:

| Indices | Field | Decoder Action | Clamp Range |
|---------|-------|---------------|-------------|
| 0-7 | EQ gains (dB) | → `eq[i] = gain/kMaxGainDB` | [-12, +12] dB |
| 8 | Target LUFS | → `loudness[0..2] = (target+14)/6` | [-23, -8] LUFS |
| 9 | True-peak ceiling | → `limiter[0..1] = (ceiling+1)/0.5` | [-3, -0.1] dBTP |
| 10 | Compressor gate | (not projected, telemetry only) | — |
| 11-28 | 3×6 compressor params | → `compParams[0..2]` full set | threshold [-40,-6], ratio [1,20], attack [0.1,100], release [10,500], makeup [0,12], knee [0,12] |
| 29 | Exciter gate | (not projected) | — |
| 30-33 | 2×2 saturation | (not projected) | — |
| 34 | Stereo gate | (not projected) | — |
| 35-38 | 2×2 stereo width | → `stereo[0..1]` | [-1, 1] |
| 39 | Limiter aggressiveness | (telemetry) | — |
| 40 | Expected gain reduction | (telemetry) | — |
| 41-43 | Character logits | (argmax → transparent/balanced/aggressive) | — |

**✅ VERDICT: The decoding is correct. All values are properly clamped to valid DSP ranges. Non-finite inputs are coerced to 0. The EQ round-trip (gain→normalized→gain) is lossless within the clamp range.**

### 2.2 Safety Policy Validation

The `NeuralMasteringSafetyPolicy` (`src/Core/NeuralMasteringSafetyPolicy.cpp:335-457`) provides a multi-layered safety net:

| Gate | Check | Hard Reject? |
|------|-------|-------------|
| Schema version mismatch | Plan schema vs expected | ✅ Yes |
| Audio callback runtime | Runtime mode prohibited | ✅ Yes |
| Invalid timestamp | Future/expired plan | ✅ Yes |
| Non-finite values | NaN/Inf in targets/deltas | ✅ Yes |
| Target out of range | [-1,1] bounds | ✅ Yes |
| Delta out of range | [-1,1] bounds | ✅ Yes |
| Illegal mask | Empty mask (no controls) | ✅ Yes |
| High-risk mask | harmonic=true or limiter=true | ✅ Yes |
| Low confidence | < 0.75 (configurable) | ❌ No (ReviewOnly fallback) |
| Abstain | Model abstained | ❌ No (DeterministicBaseline) |
| Review only | Model requested review | ❌ No (ReviewOnly fallback) |
| Stale plan | > maxPlanAgeFrames old | ❌ No (LastSafeHold) |

Default config (`defaultConfig()`):
- **Delta per plan limits:** EQ 0.15, Dynamics 0.12, Stereo 0.10, Harmonic 0.08, Limiter 0.05, Loudness 0.10 — all others 0.10
- **High-risk controls blocked:** harmonic=true, limiter=true (masked OFF by default)
- **Min confidence:** 0.75

**✅ VERDICT: The safety policy is well-designed and correctly implemented. The per-dimension delta limits prevent abrupt parameter jumps. The high-risk masking of harmonic and limiter controls is a prudent default for an experimental model.**

### 2.3 Plan Completeness

The decoded plan covers all major mastering domains:

| Domain | Covered? | Details |
|--------|----------|---------|
| Equalization | ✅ 8 bands | 60, 120, 250, 500, 1k, 2.5k, 5k, 10k Hz — 1 octave spacing |
| Dynamics | ✅ 3 bands | Full 6-param per band (threshold, ratio, attack, release, makeup, knee) |
| Stereo width | ✅ 2 regions | Low + Mid-High |
| Loudness | ✅ Target LUFS | -23 to -8 LUFS range |
| Limiting | ✅ Ceiling | -3 to -0.1 dBTP (telemetry only by default) |
| Saturation | ⚠️ Model emits | Masked OFF by default (high-risk) |
| Character | ✅ 3-class | transparent/balanced/aggressive |

**⚠️ GAP: No genre awareness in the neural path.** The heuristic `ChainPlanExecutor` (`timerCallback()` every 30s) does genre classification and plan execution, but the SonicMaster model makes decisions without genre context. This means the same waveform could receive the same treatment regardless of whether it's classical, hip-hop, or metal — the model must infer genre from the waveform alone.

### 2.4 Target LUFS Semantics

The `target_lufs` field (decision index 8) is semantically ambiguous. The MCP tool description correctly states: *"target_lufs is the mastering TARGET (echoing the caller-supplied target_lufs param), NOT a measurement of the input's loudness"*. The response includes warnings:
- `"target_lufs_is_requested_target_not_input_loudness_measurement"`
- `"target_lufs_semantics": "requested_mastering_target_not_input_loudness_measurement"`

**✅ VERDICT: Semantics are correctly documented in all user-facing surfaces. The decoder's mapping `(targetLufs + 14.0) / 6.0` is correct for the [-23, -8] range with neutral point -14 mapping to 0.**

---

## 3. Parameter Implementation Audit

### 3.1 Signal Chain Order

The `AutoMasteringEngine::processBlock()` implements this 11-stage chain (`src/Core/AutoMasteringEngine.cpp:137-211`):

```
Stage  1: MSMatrix::encode          L/R → M/S
Stage  2: MultibandSplitter          4-band Linkwitz-Riley (80/250/5k Hz)
Stage  3: MultibandDynamicsProcessor Per-band VCA compression
Stage  4: Band summation             4 bands → stereo M/S
Stage  5: AdaptiveEQ                 32-band parametric EQ
Stage  6: StereoImager               Frequency-dependent M/S width
Stage  7: HarmonicExciter            Optional tanh soft-sat
Stage  8: LoudnessNormalizer         LUFS → target correction gain
Stage  9: MSMatrix::decode           M/S → L/R
Stage 10: BrickwallLimiter           4ms lookahead, ISP detection
Stage 11: Metering                   LUFS, True Peak, Spectrum, Stereo
```

**✅ VERDICT: The signal chain order follows professional mastering best practices.** Key design decisions:

- **Dynamics BEFORE EQ (stages 3→5):** Compression in M/S domain before EQ prevents EQ boosts from triggering additional compression. Standard mastering practice.
- **Normalizer BEFORE limiter (stage 8→10):** The LUFS-1 fix (`AutoMasteringEngine.cpp:176-181`) correctly places the loudness normalizer before the brickwall limiter. This prevents the normalizer's gain from pushing an already-limited signal above the dBTP ceiling.
- **MS decode BEFORE limiter (stage 9→10):** The MSDECODE-1 fix (`AutoMasteringEngine.cpp:184-193`) correctly decodes M/S to L/R before the limiter and meters. Previously the limiter enforced the ceiling on M/S, but L = mid+side can sum to +6 dBFS after decode, causing clipping. The comment documents this precisely: *"the model eval reported a safe -0.91 dBTP while the delivered output clipped at +3.45 dBFS"*.

### 3.2 EQ Parameter Application

`applyValidatedPlan()` (`AutoMasteringEngine.cpp:366-378`) applies only 8 of 32 EQ bands:

```cpp
for (int band = 0; band < static_cast<int>(kSonicMasterEqGainCount); ++band)
    eq_.setBandGain(band, std::clamp(plan.projectedTargets.eq[band]
                                     * AdaptiveEQ::kMaxGainDB,
                                     -AdaptiveEQ::kMaxGainDB,
                                     AdaptiveEQ::kMaxGainDB));
```

Bands 8-31 are preserved from the genre translator's warm-start (`EQParameterTranslator`). The AUDIT-2 comment notes: *"Previously the loop ran to kNumBands and force-wrote bands 8..31 to 0 dB, wiping the genre character (warmth/presence) the translator had set."*

**✅ VERDICT: Correct. The model controls 8 bands; the other 24 preserve the heuristic/genre character.**

### 3.3 Dynamics Parameter Application

`applyValidatedPlan()` (`AutoMasteringEngine.cpp:380-414`) applies 3 of 4 dynamics bands. Band 3 (High) stays on heuristic defaults.

When `hasCompParams = true` (SonicMaster decisions), all 6 parameters per band are applied from the `compParams` sidecar. When false, only threshold and ratio are derived from the normalized dynamics array, and other params are preserved.

**✅ VERDICT: Correct. The `compParams` sidecar is properly populated by `decodeSonicMasterDecision()` and correctly consumed by `applyValidatedPlan()`.**

### 3.4 Stereo Parameter Application

`applyValidatedPlan()` (`AutoMasteringEngine.cpp:416-425`) applies 2 of 4 stereo regions with correct mapping: `width = clamp(1.0 + normalized, 0.0, 2.0)`.

### 3.5 Harmonic and Limiter Masking

Both are correctly gated by `appliedMask.harmonic` and `appliedMask.limiter`, which are `false` by default (the decoder explicitly sets `out.appliedMask.limiter = false` at line 75). This matches the `DeterministicBaseline` posture.

### 3.6 🐛 BUG: MCP Telemetry Reports Wrong Compressor Values

**Severity: MEDIUM (telemetry/docs only — DSP application is correct)**

**Location:** `src/AI/MCPToolHandler.cpp:5740-5759`

In the `sonicmaster_decision` MCP tool's `actual_engine_mapping` section, compressor attack/release/makeup/knee values are read from the current DSP state rather than from the decoded model plan:

```cpp
auto& dynamics = p.getAutoMasteringEngine().getDynamics();
for (std::size_t b = 0; b < more_phi::kSonicMasterCompBandCount; ++b)
{
    const auto current = dynamics.getBandParams(static_cast<int>(b));  // ← READS CURRENT DSP STATE
    // ...
    engineDynamics.push_back({
        {"attackMs", json{{"value", current.attackMs}, {"source", "current_engine_value_preserved"}}},
        {"releaseMs", json{{"value", current.releaseMs}, {"source", "current_engine_value_preserved"}}},
        {"makeupDb", json{{"value", current.makeupDB}, {"source", "current_engine_value_preserved"}}},
        {"kneeDb", json{{"value", current.kneeDB}, {"source", "current_engine_value_preserved"}}},
        {"direct_model_controls", json::array({"thresholdDb", "ratio"})},
        {"raw_telemetry_only_controls", json::array({"attackMs", "releaseMs", "makeupDb", "kneeDb"})},
    });
}
```

**The problem:** The model DOES emit attack/release/makeup/knee values (they're in the 44-float decision vector at indices o+2 through o+5). The decoder DOES decode them into the `compParams` sidecar. But the MCP telemetry reads the current DSP values instead of the decoded sidecar, and labels them "current_engine_value_preserved" and "raw_telemetry_only_controls" — misleading labels since they ARE available from the model.

**Impact:** An AI assistant reading the `sonicmaster_decision` response sees the OLD DSP values for attack/release/makeup/knee, not the model's actual recommendations. When the assistant then calls `apply_mastering_plan`, `applyValidatedPlan()` correctly uses the `compParams` sidecar — so the DSP IS set correctly, but the assistant's telemetry was wrong.

**Fix:** The `engineMapping` should read from `plan.compParams[b]` (available from the `ValidatedNeuralMasteringPlan` returned by `requestDecisionNow`) instead of `dynamics.getBandParams()`:

```cpp
const auto& cp = plan.compParams[b];
engineDynamics.push_back({
    // ...
    {"attackMs", cp.attackMs},
    {"releaseMs", cp.releaseMs},
    {"makeupDb", cp.makeupDb},
    {"kneeDb", cp.kneeDb},
    {"direct_model_controls", json::array({"thresholdDb", "ratio", "attackMs", "releaseMs", "makeupDb", "kneeDb"})},
});
```

---

## 4. Quality Assessment

### 4.1 Safety Posture

The safety policy is the strongest aspect of this implementation:

- **Delta limiting:** Per-plan parameter changes are clamped to small increments (EQ: 0.15, Dynamics: 0.12, etc.), preventing jarring transitions.
- **Last-safe hold:** On any validation failure, the policy falls back to the last accepted plan.
- **High-risk masking:** Harmonic exciter and brickwall limiter are masked OFF by default, requiring explicit user opt-in.
- **Confidence gating:** Plans below 0.75 confidence trigger ReviewOnly fallback.
- **Consecutive failure auto-disable:** After 3 consecutive inference failures, the engine auto-disables (`SonicMasterAnalysisEngine.cpp:259-264`).

### 4.2 Model Quality Limitations

The design spec (`docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md:40`) documents:

> **Quality status:** Research-only; failed 4/9 release gates (EQ MAE 2.12 dB, TP 0.80 dBTP)

This means the model's EQ predictions have a mean absolute error of 2.12 dB per band — significant enough that a human mastering engineer would notice. The true-peak error of 0.80 dBTP is also material for a limiter ceiling decision.

**The safety policy can only partially mitigate this:** it prevents dangerous parameter swings but cannot fix systematically inaccurate model outputs. A model that consistently over-EQs at 2.5 kHz will pass the safety gates (since each plan's deltas are within limits) but produce poor-sounding results.

### 4.3 Potential Artifacts and Side Effects

| Issue | Risk | Mitigation |
|-------|------|------------|
| Linear resampling aliasing (>9 kHz at 96k) | Low-Medium | Model trained on same aliasing; self-consistent |
| Peak-normalization erases loudness context | High | Documented; model uses caller-supplied target_lufs |
| 5.94s window too short for genre/LRA context | Medium | Heuristic genre classifier runs separately |
| Compressor time-constant mismatch | Low | Full 6-param sidecar matches model's intent |
| MS encode/decode level shift | None | MSDECODE-1 fix resolved; limiter now on L/R |
| Normalizer→Limiter order | None | LUFS-1 fix resolved correct ordering |
| Model quality (EQ MAE 2.12 dB) | High | Safety policy limits per-plan deltas but cannot fix bias |

### 4.4 Threading Correctness

The thread model is correctly implemented:

- **Audio thread:** Only `capture()` — a lock-free SPSC ring write. No allocation, no locks. ✅
- **Analysis thread:** Owns the ONNX session exclusively. `runCycle()` serialized against `requestDecisionNow()` via `inferMutex_`. ✅
- **Message thread:** `applyValidatedPlan()` called via `juce::MessageManager::callAsync()`. ✅
- **Teardown order:** `release()` signals stop → joins thread → then destroys ring. ✅

### 4.5 Memory and Allocation

- `AudioCaptureRing`: Pre-allocated power-of-2 stereo buffer (8s × 192kHz ≈ 3M samples, ~12 MB). ✅
- `SonicMasterAnalysisEngine` scratch buffers: ~262k samples each for L/R model + interleaved (~3 MB total). Allocated in `prepare()`, not on the audio thread. ✅
- `SonicMasterHttpInferenceSource::infer()`: `juce::MemoryBlock` copies the 2MB interleaved buffer — this is on the analysis thread, acceptable. ✅
- No heap allocations on the audio thread. ✅

---

## 5. Findings Summary

### Correct Implementations ✅

1. **Thread architecture:** Three-domain separation with correct lock-free primitives
2. **Safety policy:** Multi-layered validation with delta clamping, confidence gating, high-risk masking
3. **Signal chain order:** MS encode→dynamics→EQ→stereo→normalize→MS decode→limiter→meter (professional standard)
4. **MSDECODE-1 fix:** Limiter and meters now on L/R, not M/S
5. **LUFS-1 fix:** Normalizer before limiter prevents ceiling violation
6. **EQ band scoping:** Model controls 8 bands; genre translator preserves bands 8-31
7. **Dynamics band scoping:** Model controls 3 bands; band 3 (High) stays heuristic
8. **Full compressor sidecar:** All 6 params decoded and applied when available
9. **Teardown order:** Thread joined before any owned state destroyed
10. **Peak-normalization documentation:** AUDIT-7 comments correctly explain the loudness limitation
11. **MCP tool warnings:** Response includes semantic disclaimers about target_lufs and telemetry

### Bugs and Issues 🐛

| ID | Severity | Description | File:Line |
|----|----------|-------------|-----------|
| BUG-1 | MEDIUM | MCP `sonicmaster_decision` engineMapping reads current DSP compressor values instead of decoded `compParams` sidecar — AI assistant sees wrong attack/release/makeup/knee values | `src/AI/MCPToolHandler.cpp:5740-5759` |

### Design Limitations ⚠️

| ID | Severity | Description |
|----|----------|-------------|
| LIM-1 | HIGH | Peak-normalization destroys absolute loudness — model cannot measure input LUFS |
| LIM-2 | MEDIUM | 5.94s window insufficient for Integrated LUFS/LRA per ITU-R BS.1770 |
| LIM-3 | MEDIUM | Linear resampling introduces aliasing above ~9 kHz (intentional, self-consistent) |
| LIM-4 | MEDIUM | No genre awareness in neural path — model must infer genre from waveform alone |
| LIM-5 | HIGH | Model quality: failed 4/9 release gates (EQ MAE 2.12 dB, TP 0.80 dBTP) |

---

## 6. Recommendations

### Immediate (bug fix)

1. **Fix BUG-1:** Update `MCPToolHandler::sonicmasterDecision()` engineMapping to read compressor attack/release/makeup/knee from `plan.compParams[b]` instead of `dynamics.getBandParams()`. Also update the `direct_model_controls` list to include all 6 params.

### Short-term (quality improvements)

2. **Add input loudness context:** Before calling `source_->setTargetLufs()`, measure the input's integrated LUFS from the `AutoMasteringEngine`'s `LUFSMeter` and pass both the measured input LUFS and the desired target LUFS to the model. This gives the model the absolute level context it currently lacks.

3. **Increase analysis window:** Buffer longer audio (e.g., 30s rolling window) and feed it to the model as multiple overlapping 6s segments, then ensemble the decisions. This would improve LRA-adjacent decisions.

4. **Model quality:** Prioritize retraining to close the 4/9 failed release gates, especially EQ MAE (2.12 dB → target <1.0 dB) and true-peak accuracy (0.80 dBTP → target <0.3 dBTP).

### Long-term (architecture)

5. **Genre-aware neural path:** Feed the `GenreClassifier` output as an additional input to the neural model, or use it to select genre-specific post-processing of the model's decisions.

6. **Replace linear resampling:** When the model is retrained, use a polyphase FIR resampler and retrain on the cleanly resampled audio.

---

## 7. Overall Assessment

**The SonicMaster AI mastering pipeline is a competent, safety-conscious implementation whose architecture and DSP integration are production-quality.** The code is well-structured, well-documented (the AUDIT comments are excellent), and correctly implements professional mastering signal flow. The safety policy is the standout feature — it provides robust protection against model errors.

However, the system is fundamentally limited by its neural model: the peak-normalization preprocessing removes critical loudness context, the 6-second window is too short for program-level decisions, and the model's documented accuracy (EQ MAE 2.12 dB) means it cannot yet match a skilled human mastering engineer. These are model-training problems, not implementation problems — the pipeline itself is correctly built to host a better model when one becomes available.

**Grade:** B+ for architecture and implementation; C+ for mastering competency (limited by model quality).

---

## 8. Post-Audit Fixes Applied (2026-06-23)

### BUG-1 (MEDIUM) — MCP telemetry reads wrong compressor source

**File:** `src/AI/MCPToolHandler.cpp:5739-5759`

The `sonicmaster_decision` MCP tool's `actual_engine_mapping.dynamics_bands` section was reading compressor attack/release/makeup/knee from `dynamics.getBandParams()` (current DSP state) instead of the model's decoded `compParams` sidecar. This meant an AI assistant reading the response saw stale/heuristic DSP values labeled as "current_engine_value_preserved" when the model had actually produced different values.

**Fix:** Changed the `engineMapping` dynamics loop to read from `plan.compParams[b]` directly. Removed the `auto& dynamics = p.getAutoMasteringEngine().getDynamics()` reference and the nested JSON `{"value":..., "source":...}` wrapper. Updated `direct_model_controls` to list all 6 params (threshold, ratio, attack, release, makeup, knee). Removed the misleading `raw_telemetry_only_controls` key.

### BUG-2 (HIGH) — compParams sidecar dropped by safety policy verdict

**Files:**
- `src/Core/NeuralMasteringTypes.h:210` — Added `compParams` and `hasCompParams` to `NeuralMasteringPlanCandidate`
- `src/AI/SonicMasterAnalysisEngine.cpp:94-95` — `makeSafetyCandidate()` now copies compParams from the decoded plan
- `src/Core/NeuralMasteringSafetyPolicy.cpp:345-346` — `validate()` now propagates compParams from candidate to `result.plan`

The `NeuralMasteringPlanCandidate` struct (the transport between the decoder and the safety policy) had no fields for `compParams`/`hasCompParams`. The `makeSafetyCandidate()` helper only copied `targets`, `deltas`, and `editableMask`. The `validate()` function constructed a fresh `ValidatedNeuralMasteringPlan` without ever copying compressor params. This meant the model's full 6-param compressor decisions (attack, release, makeup, knee) were silently dropped every time a plan passed through the safety policy — affecting both the autonomous `runCycle()` → `applyValidatedPlan()` path and the on-demand `requestDecisionNow()` path. `applyValidatedPlan()` always fell through to the `else` branch (threshold/ratio-only from normalized dynamics), losing the model's carefully chosen time constants and gain staging.

**Fix:** Added `compParams` and `hasCompParams` to `NeuralMasteringPlanCandidate`, copy them in `makeSafetyCandidate()`, and propagate them to the verdict plan in `validate()`. The `lastSafePlan_` stored by the safety policy now preserves compParams for fallback paths. The `applyFallback()` paths correctly handle compParams: `LastSafeHold` preserves the previous plan's compParams; `DeterministicBaseline`/`TransparentBypass`/`Reject` correctly clear them.

**Verification:** Full Release build succeeded. All 30 related tests pass (decodeSonicMasterDecision, NeuralMasteringSafety, AutoMasteringEngine, SonicMasterDecisionRunner, QualitySafetyAgent).

---

*Audit produced by automated codebase analysis of the More-Phi VST3 plugin repository.*
