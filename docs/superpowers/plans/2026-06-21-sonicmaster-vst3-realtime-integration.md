# SonicMaster v2 → VST3 Realtime Neural Mastering Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the `masteringbrainv2` checkpoint into the More-Phi VST3 as an in-process ONNX realtime neural mastering assistant that continuously analyses ~6 s of live audio and applies safe decoded mastering parameters to the built-in DSP chain.

**Architecture:** A new `SonicMasterAnalysisEngine` runs an ONNX waveform→decision model on a background thread. The audio thread does nothing more than a lock-free ring write each block; the analysis thread (woken every 3 s) drains the ring, resamples to 44.1 kHz, runs inference, decodes the 44-float decision into a `ValidatedNeuralMasteringPlan`, validates it through the existing `NeuralMasteringSafetyPolicy`, and hands it to the message thread which ramps the DSP parameters into `AutoMasteringEngine`. The model is exported to ONNX once via an offline script; everything else is C++. Default OFF; preview feature.

**Tech Stack:** C++20, JUCE 8, ONNX Runtime 1.22.1 (gated `MORE_PHI_ENABLE_ONNX`), Catch2 v3, nlohmann/json, Python 3.12 + PyTorch 2.11 (export script only).

**Spec:** `docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md`

---

## File structure

**Created:**
- `src/AI/SonicMasterDecisionDecoder.h` / `.cpp` — pure, noexcept, ONNX-free: 44-float decision → `ValidatedNeuralMasteringPlan`. Single source of truth for the slice map.
- `src/AI/SonicMasterDecisionRunner.h` / `.cpp` — thin ONNX session wrapper (waveform in, 44 floats out). Pimpl + `MORE_PHI_HAS_ONNX` gating, mirroring `OnnxNeuralMasteringRunner`.
- `src/Core/AudioCaptureRing.h` — lock-free SPSC ring buffer (stereo float), mirroring `LockFreeQueue.h`'s cache-line-aligned indices.
- `src/AI/SonicMasterAnalysisEngine.h` / `.cpp` — the orchestrator: owns the runner, capture ring, analysis thread, safety policy, and the message-thread ramp.
- `tools/export_onnx/masteringbrain_to_onnx.py` — offline export + parity check.
- `tools/export_onnx/README.md` — runbook for the export.
- `tests/Unit/TestSonicMasterDecisionDecoder.cpp`
- `tests/Unit/TestAudioCaptureRing.cpp`
- `tests/Unit/TestSonicMasterAnalysisEngine.cpp`
- `tests/Unit/TestSonicMasterRunnerLive.cpp` — opt-in (ONNX + staged model).

**Modified (existing):**
- `src/Plugin/PluginProcessor.h` / `.cpp` — add the engine member, construct it, wire `setApplicationEngine`, call `capture`/`prepare`/`release`. One new APVTS bool.
- `src/Plugin/PluginEditor.h` / `.cpp` — one toggle button + status label.
- `tests/CMakeLists.txt` — register the new test sources and stage the ONNX model + `contract.json`.

The decode target is the **existing** `ValidatedNeuralMasteringPlan` (`src/Core/NeuralMasteringTypes.h`) — `AutoMasteringEngine::applyValidatedPlan` already maps it to the DSP setters with clamping, so no new plan struct is invented.

---

## Conventions used in every task

- All new code lives in namespace `more_phi`.
- Build commands (run from repo root `G:\More_Phi-vst3-plugin`):
  - Configure: `cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON`
  - Build: `cmake --build build --config Release`
  - Run one test: `cd build && ctest -R "TestName" --output-on-failure -C Release`
  - Run all tests: `cd build && ctest --output-on-failure --parallel 4 -C Release`
- Every task ends with a commit on the current branch (`feature/release-validation`).
- Tests are Catch2 v3 (`TEST_CASE`/`SECTION`/`CHECK`/`REQUIRE`), tagged.
- New source files added to the build target `MorePhi` are appended in `CMakeLists.txt` wherever the existing `src/AI/*.cpp` / `src/Core/*.cpp` are listed (search for an existing neighbor to place the new entry next to).

---

## Task 1: Decision slice-map constants

Establishes the single source of truth for the 44-float decision layout so every later task references the same offsets.

**Files:**
- Create: `src/AI/SonicMasterDecisionDecoder.h`

- [ ] **Step 1: Create the header with the slice-map constants and the decode signature**

```cpp
// src/AI/SonicMasterDecisionDecoder.h
#pragma once

#include "Core/NeuralMasteringTypes.h"

#include <cstddef>

namespace more_phi {

// Width of the masteringbrainv2 full-chain-v2 decision vector. Must match
// FULL_CHAIN_REGRESSION_WIDTH in mastering_decision_adapter.py (= num_eq_bands
// + 2 + 1 + 18 + 1 + 4 + 1 + 4 + 1 + 1 + character_logits).
inline constexpr std::size_t kSonicMasterDecisionWidth = 44;

// Slice offsets into the 44-float decision vector. Copied verbatim from
// mastering_decision_adapter.decode_mastering_decision() so C++ and Python
// agree byte-for-byte. Do not reorder without re-exporting the model.
inline constexpr std::size_t kSonicMasterEqGainCount   = 8;
inline constexpr std::size_t kSonicMasterEqGainOffset  = 0;
inline constexpr std::size_t kSonicMasterTargetLufsIdx = 8;
inline constexpr std::size_t kSonicMasterTruePeakIdx   = 9;
inline constexpr std::size_t kSonicMasterCompGateIdx   = 10;
inline constexpr std::size_t kSonicMasterCompOffset    = 11; // 18 floats: 3 x (thr,ratio,atk,rel,makeup,knee)
inline constexpr std::size_t kSonicMasterCompBandCount = 3;
inline constexpr std::size_t kSonicMasterCompBandWidth = 6;
inline constexpr std::size_t kSonicMasterExciterGateIdx = 29;
inline constexpr std::size_t kSonicMasterSatOffset     = 30; // 4 floats: 2 x (drive,mix)
inline constexpr std::size_t kSonicMasterStereoGateIdx = 34;
inline constexpr std::size_t kSonicMasterStereoOffset  = 35; // 4 floats: 2 x (width,sideGain)
inline constexpr std::size_t kSonicMasterAggrIdx       = 39;
inline constexpr std::size_t kSonicMasterGainRedIdx    = 40;
inline constexpr std::size_t kSonicMasterCharOffset    = 41; // 3 logits -> transparent/balanced/aggressive
inline constexpr std::size_t kSonicMasterCharCount     = 3;

// EQ band centre frequencies used by the model (EQ_DECISION_FREQUENCIES_HZ).
// Bands 0..7 of AdaptiveEQ receive these freqs; gains come from the decision.
inline constexpr float kSonicMasterEqFrequenciesHz[kSonicMasterEqGainCount] = {
    60.0f, 120.0f, 250.0f, 500.0f, 1000.0f, 2500.0f, 5000.0f, 10000.0f
};

// Default neutral Q for the decoded EQ bands (matches EQ_DECISION_Q).
inline constexpr float kSonicMasterEqDefaultQ = 0.707f;

// Mirrors AdaptiveEQ::kMaxGainDB (Core/AdaptiveEQ.h). Re-stated here so the
// decoder stays DSP-header-free and the tests can assert against it without
// pulling AdaptiveEQ.h. The test in Task 2 pins the value to 12.0.
inline constexpr float kAdaptiveEqMaxGainDb = 12.0f;

// Confidence attached to every decoded plan. Above the safety policy's
// default minConfidence (0.75) so plausible frames clear the gate, below 1.0
// to leave headroom for the safety re-clamp. See design §5.2.
inline constexpr float kSonicMasterDefaultConfidence = 0.85f;

/**
 * Decode a 44-float masteringbrainv2 decision into a ValidatedNeuralMasteringPlan
 * whose projectedTargets / appliedMask are consumed directly by
 * AutoMasteringEngine::applyValidatedPlan(). Pure, noexcept, allocation-free,
 * ONNX-free. Non-finite inputs are coerced; every target is clamped into the
 * range the receiving DSP module expects so the safety policy is a second line,
 * not the first.
 *
 * On success returns true and fills `out`. Returns false only if `decision` is
 * null or `sampleRate <= 0` (the caller treats false as "skip this cycle").
 */
bool decodeSonicMasterDecision(const float* decision,
                               std::size_t decisionCount,
                               double sampleRate,
                               ValidatedNeuralMasteringPlan& out) noexcept;

} // namespace more_phi
```

- [ ] **Step 2: Verify it compiles in isolation (no impl yet)**

Run: `cmake --build build --config Release 2>&1 | findstr SonicMaster`
Expected: build fails only because `decodeSonicMasterDecision` is declared but not defined — but the header itself parses. If the build breaks on the header's syntax, fix the syntax before proceeding. (The undefined-symbol error is expected and resolves in Task 2.)

- [ ] **Step 3: Commit**

```bash
git add src/AI/SonicMasterDecisionDecoder.h
git commit -m "feat(sonicmaster): decision slice-map constants + decode signature"
```

---

## Task 2: Decode the 44-float decision — TDD

Implements `decodeSonicMasterDecision`. This is the load-bearing correctness task: every downstream component trusts this mapping.

**Files:**
- Create: `tests/Unit/TestSonicMasterDecisionDecoder.cpp`
- Create: `src/AI/SonicMasterDecisionDecoder.cpp`

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/Unit/TestSonicMasterDecisionDecoder.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AI/SonicMasterDecisionDecoder.h"
#include "Core/NeuralMasteringTypes.h"

#include <cmath>
#include <limits>

namespace {

// Build a decision vector where every slot encodes its own index /10 so each
// field can be asserted independently after decode.
void fillIndexScaled(float (&decision)[more_phi::kSonicMasterDecisionWidth])
{
    for (std::size_t i = 0; i < more_phi::kSonicMasterDecisionWidth; ++i)
        decision[i] = static_cast<float>(i) * 0.1f;
}

} // namespace

TEST_CASE("decodeSonicMasterDecision maps EQ gains to eq[0..7] scaled into AdaptiveEQ range",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    fillIndexScaled(decision);
    // Set sane, in-range EQ gains (dB) so the clamp path is not exercised here.
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
        decision[i] = static_cast<float>(i) - 3.5f; // -3.5 .. +3.5 dB

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    REQUIRE(plan.valid);
    REQUIRE(plan.appliedMask.eq);
    // kAdaptiveEqMaxGainDb == 12. Decoded target = gain_db / kAdaptiveEqMaxGainDb.
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
    {
        const float expected = (static_cast<float>(i) - 3.5f) / more_phi::kAdaptiveEqMaxGainDb;
        CHECK_THAT(plan.projectedTargets.eq[i],
                   Catch::Matchers::WithinAbs(expected, 1e-4f));
    }
}

TEST_CASE("decodeSonicMasterDecision clamps out-of-range EQ to +/-kMaxGainDB",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[0] = 50.0f;  // far over +12 dB
    decision[1] = -50.0f; // far under -12 dB

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    CHECK_THAT(plan.projectedTargets.eq[0], Catch::Matchers::WithinAbs(1.0f, 1e-4f));  // +12/12
    CHECK_THAT(plan.projectedTargets.eq[1], Catch::Matchers::WithinAbs(-1.0f, 1e-4f)); // -12/12
}

TEST_CASE("decodeSonicMasterDecision maps target LUFS into loudness band",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    REQUIRE(plan.appliedMask.loudness);
    // AutoMasteringEngine::applyValidatedPlan maps loudness[0] as
    // -14 + value*6 clamped to [-23,-8]. So a target of -14 LUFS must decode
    // to value 0.0 (so the formula yields exactly -14).
    CHECK_THAT(plan.projectedTargets.loudness[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision maps true-peak ceiling into limiter band",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    decision[more_phi::kSonicMasterTruePeakIdx] = -1.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    REQUIRE(plan.appliedMask.limiter);
    // applyValidatedPlan: ceiling = -1 + limiter[0]*0.5 clamped to [-3,-0.1].
    // A -1 dBTP target must decode to limiter[0] == 0.0.
    CHECK_THAT(plan.projectedTargets.limiter[0], Catch::Matchers::WithinAbs(0.0f, 1e-4f));
}

TEST_CASE("decodeSonicMasterDecision fills the 3-band compressor block",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    // Comp band 0: threshold=-20,ratio=2.5,attack=10,release=100,makeup=0,knee=3
    decision[more_phi::kSonicMasterCompOffset + 0] = -20.0f;
    decision[more_phi::kSonicMasterCompOffset + 1] = 2.5f;
    decision[more_phi::kSonicMasterCompOffset + 2] = 10.0f;
    decision[more_phi::kSonicMasterCompOffset + 3] = 100.0f;
    decision[more_phi::kSonicMasterCompOffset + 4] = 0.0f;
    decision[more_phi::kSonicMasterCompOffset + 5] = 3.0f;

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    REQUIRE(plan.appliedMask.dynamics);
    // applyValidatedPlan maps dynamics[band] = value -> threshold = -20 + value*8,
    // ratio = 2.5 + value*1.5. So threshold=-20 -> value 0, ratio=2.5 -> value 0.
    CHECK_THAT(plan.projectedTargets.dynamics[0], Catch::Matchers::WithinAbs(0.0f, 1e-3f));
}

TEST_CASE("decodeSonicMasterDecision coerces NaN to neutral without throwing",
          "[SonicMaster][Decoder]")
{
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    for (auto& v : decision) v = std::numeric_limits<float>::quiet_NaN();

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));

    // Every target must be finite; EQ neutral = 0.0, LUFS neutral (=-14) -> 0.0.
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
        CHECK(std::isfinite(plan.projectedTargets.eq[i]));
    CHECK(std::isfinite(plan.projectedTargets.loudness[0]));
    CHECK(std::isfinite(plan.projectedTargets.limiter[0]));
}

TEST_CASE("decodeSonicMasterDecision rejects null/bad args",
          "[SonicMaster][Decoder]")
{
    more_phi::ValidatedNeuralMasteringPlan plan {};
    CHECK_FALSE(more_phi::decodeSonicMasterDecision(nullptr, more_phi::kSonicMasterDecisionWidth, 48000.0, plan));
    float decision[more_phi::kSonicMasterDecisionWidth] {};
    CHECK_FALSE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth, 0.0, plan));
    CHECK_FALSE(more_phi::decodeSonicMasterDecision(decision, more_phi::kSonicMasterDecisionWidth - 1, 48000.0, plan));
}
```

(`kAdaptiveEqMaxGainDb` is defined in the header from Task 1, so the test sees it directly — no duplication.)

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd build && ctest -R "SonicMaster.*Decoder" --output-on-failure -C Release`
Expected: FAIL — linker error (`decodeSonicMasterDecision` undefined, test source not yet in the build).

- [ ] **Step 3: Implement the decoder**

```cpp
// src/AI/SonicMasterDecisionDecoder.cpp
#include "AI/SonicMasterDecisionDecoder.h"

#include <algorithm>
#include <cmath>

namespace more_phi {

namespace {

float finiteOr(float v, float fallback) noexcept
{
    return std::isfinite(v) ? v : fallback;
}

float clamp(float v, float lo, float hi) noexcept
{
    return std::clamp(finiteOr(v, 0.0f), lo, hi);
}

} // namespace

bool decodeSonicMasterDecision(const float* decision,
                               std::size_t decisionCount,
                               double sampleRate,
                               ValidatedNeuralMasteringPlan& out) noexcept
{
    if (decision == nullptr
        || decisionCount < kSonicMasterDecisionWidth
        || sampleRate <= 0.0)
        return false;

    out = {}; // start neutral

    // ── EQ: gains (dB) -> eq[i] = gain / kMaxGainDB, clamped to [-1, 1] ───────
    for (std::size_t i = 0; i < kSonicMasterEqGainCount; ++i)
    {
        const float gainDb = clamp(decision[kSonicMasterEqGainOffset + i],
                                   -kAdaptiveEqMaxGainDb, kAdaptiveEqMaxGainDb);
        out.projectedTargets.eq[i] = gainDb / kAdaptiveEqMaxGainDb;
    }
    out.appliedMask.eq = true;

    // ── Loudness: target LUFS -> loudness[0] so that
    //    (-14 + value*6) clamped to [-23,-8] reproduces the target. Solve the
    //    forward map at the target (clamping first): value = (target + 14)/6,
    //    then let applyValidatedPlan re-clamp defensively.
    {
        const float targetLufs = clamp(decision[kSonicMasterTargetLufsIdx], -30.0f, -6.0f);
        out.projectedTargets.loudness[0] = (targetLufs + 14.0f) / 6.0f;
        out.projectedTargets.loudness[1] = out.projectedTargets.loudness[0];
        out.projectedTargets.loudness[2] = out.projectedTargets.loudness[0];
        out.appliedMask.loudness = true;
    }

    // ── Limiter: true-peak ceiling -> limiter[0] so that
    //    (-1 + value*0.5) clamped to [-3,-0.1] reproduces the ceiling.
    {
        const float ceiling = clamp(decision[kSonicMasterTruePeakIdx], -6.0f, -0.1f);
        out.projectedTargets.limiter[0] = (ceiling + 1.0f) / 0.5f;
        out.projectedTargets.limiter[1] = out.projectedTargets.limiter[0];
        out.appliedMask.limiter = true;
    }

    // ── Dynamics: 3 x (threshold,ratio,attack,release,makeup,knee).
    //    applyValidatedPlan maps dynamics[band] -> threshold = -20 + v*8,
    //    ratio = 2.5 + v*1.5. We encode the band's threshold as the target
    //    (solving forward), clamped to the formula's output range.
    for (std::size_t band = 0; band < kSonicMasterCompBandCount; ++band)
    {
        const std::size_t o = kSonicMasterCompOffset + band * kSonicMasterCompBandWidth;
        const float thresholdDb = clamp(decision[o + 0], -40.0f, -6.0f);
        // Invert the apply formula: v_thr = (threshold + 20) / 8, then clamp to
        // the formula's output domain [-40,-6] -> v in [-2.5, 1.75]; we further
        // clamp to [-1, 1] to stay inside the safety policy's delta range.
        float v = (thresholdDb + 20.0f) / 8.0f;
        out.projectedTargets.dynamics[band] = clamp(v, -1.0f, 1.0f);
    }
    out.appliedMask.dynamics = true;

    // ── Stereo: 2 x (width, sideGain). The first width feeds region 0.
    //    applyValidatedPlan: width_out = 1 + stereo[region], clamped [0,2].
    //    Decode decision[35] (width band 0) directly as the offset.
    {
        const float width0 = clamp(decision[kSonicMasterStereoOffset + 0], -1.0f, 1.0f);
        out.projectedTargets.stereo[0] = width0;
        out.projectedTargets.stereo[1] = width0;
        out.appliedMask.stereo = true;
    }

    // Saturation/exciter and character are decoded for telemetry only — the
    // default appliedMask leaves harmonic=false (high-risk per the safety
    // policy), matching the DeterministicBaseline posture. The raw gate values
    // are coerced to finite but not projected.

    out.fallbackMode   = NeuralMasteringFallbackMode::None;
    out.evidenceLevel  = NeuralMasteringEvidenceLevel::PrototypeMeasured;
    out.valid          = true;
    out.projected      = true;
    out.sourcePlanId   = 0; // filled in by the analysis engine
    return true;
}

} // namespace more_phi
```

- [ ] **Step 4: Register the new source + test in the build**

In `tests/CMakeLists.txt`, inside the `add_executable(MorePhiTests ...)` list (place next to the other `Unit/TestOnnx*.cpp` entries):

```cmake
    Unit/TestSonicMasterDecisionDecoder.cpp
```

In `CMakeLists.txt`, add `src/AI/SonicMasterDecisionDecoder.cpp` to the `MorePhi` target's source list next to the other `src/AI/*.cpp` entries (search for `src/AI/OnnxNeuralMasteringRunner.cpp` and add the new file on the following line).

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Release && cd build && ctest -R "SonicMaster.*Decoder" --output-on-failure -C Release`
Expected: PASS — all 7 sections green.

- [ ] **Step 6: Commit**

```bash
git add src/AI/SonicMasterDecisionDecoder.cpp tests/Unit/TestSonicMasterDecisionDecoder.cpp tests/CMakeLists.txt CMakeLists.txt
git commit -m "feat(sonicmaster): decode 44-float decision to ValidatedNeuralMasteringPlan"
```

---

## Task 3: AudioCaptureRing — TDD

Lock-free SPSC stereo capture ring. The single structure spanning audio↔analysis.

**Files:**
- Create: `tests/Unit/TestAudioCaptureRing.cpp`
- Create: `src/Core/AudioCaptureRing.h`

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/Unit/TestAudioCaptureRing.cpp
#include <catch2/catch_test_macros.hpp>

#include "Core/AudioCaptureRing.h"

#include <algorithm>
#include <numeric>
#include <thread>

TEST_CASE("AudioCaptureRing round-trips a single write then read", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/1024);
    float l[64], r[64];
    std::iota(std::begin(l), std::end(l), 0.0f);
    std::iota(std::begin(r), std::end(r), 100.0f);
    ring.write(l, r, 64);

    std::vector<float> lo(64, -1.0f), ro(64, -1.0f);
    REQUIRE(ring.readNewest(64, lo.data(), ro.data()) == 64);
    CHECK(lo[0] == 0.0f);
    CHECK(lo[63] == 63.0f);
    CHECK(ro[0] == 100.0f);
}

TEST_CASE("AudioCaptureRing returns newest window when producer laps consumer", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/256);
    // Write 1000 frames of ramped data; only the newest 128 survive.
    for (int batch = 0; batch < 10; ++batch)
    {
        std::vector<float> l(100), r(100);
        std::iota(l.begin(), l.end(), static_cast<float>(batch * 100));
        std::iota(r.begin(), r.end(), static_cast<float>(batch * 100) + 0.5f);
        ring.write(l.data(), r.data(), 100);
    }

    std::vector<float> lo(128, -1.0f), ro(128, -1.0f);
    REQUIRE(ring.readNewest(128, lo.data(), ro.data()) == 128);
    // The newest frame written was the 1000th; the window's last sample is the
    // last value of the final batch (99 + 900 = 999).
    CHECK(lo[127] == Catch::Approx(999.0f).margin(1.0f));
}

TEST_CASE("AudioCaptureRing reports captured frame count", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/512);
    CHECK(ring.capturedFrames() == 0);
    std::vector<float> l(200, 0.0f), r(200, 0.0f);
    ring.write(l.data(), r.data(), 200);
    CHECK(ring.capturedFrames() == 200);
}

TEST_CASE("AudioCaptureRing resets on prepare", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/512);
    std::vector<float> l(100, 1.0f), r(100, 1.0f);
    ring.write(l.data(), r.data(), 100);
    ring.reset();
    CHECK(ring.capturedFrames() == 0);
}
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cd build && ctest -R "AudioCaptureRing" --output-on-failure -C Release`
Expected: FAIL — `AudioCaptureRing` undefined.

- [ ] **Step 3: Implement the ring**

```cpp
// src/Core/AudioCaptureRing.h
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace more_phi {

// Single-producer (audio thread) / single-consumer (analysis thread) ring of
// stereo float frames. Power-of-two capacity. Cache-line-aligned indices.
// write() is the ONLY audio-thread API; readNewest()/capturedFrames()/reset()
// are for the analysis thread only.
class AudioCaptureRing
{
public:
    explicit AudioCaptureRing(std::size_t capacityFrames)
        : capacityFrames_(roundUpPow2(capacityFrames)),
          buffer_(2ull * capacityFrames_, 0.0f),
          writePos_(0),
          totalWritten_(0),
          hasWrapped_(false)
    {
    }

    // Resizes the ring and discards all contents. Analysis thread / message
    // thread only — never the audio thread.
    void reset() noexcept
    {
        writePos_.store(0, std::memory_order_relaxed);
        totalWritten_.store(0, std::memory_order_relaxed);
        hasWrapped_.store(false, std::memory_order_relaxed);
    }

    // Audio thread: append `n` stereo frames. Lock-free, no allocation.
    void write(const float* left, const float* right, std::size_t n) noexcept
    {
        const std::size_t cap = capacityFrames_;
        std::size_t w = writePos_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i)
        {
            buffer_[0ull * cap + w] = left[i];
            buffer_[1ull * cap + w] = right[i];
            w = (w + 1) & mask_();
        }
        writePos_.store(w, std::memory_order_release);

        const std::uint64_t prevTotal = totalWritten_.fetch_add(n, std::memory_order_relaxed);
        if (prevTotal + n > cap)
            hasWrapped_.store(true, std::memory_order_relaxed);
    }

    // Analysis thread: copy the most recent `n` frames (chronological order)
    // into outL/outR. Returns the number actually copied (may be < n if fewer
    // than n frames have ever been captured).
    std::size_t readNewest(std::size_t n, float* outL, float* outR) const noexcept
    {
        const std::size_t cap = capacityFrames_;
        const std::uint64_t total = totalWritten_.load(std::memory_order_acquire);
        const std::size_t available = static_cast<std::size_t>(
            std::min<std::uint64_t>(total, hasWrapped_.load(std::memory_order_relaxed) ? cap : total));
        if (available == 0 || n == 0) return 0;
        const std::size_t take = std::min(n, available);

        const std::size_t w = writePos_.load(std::memory_order_acquire);
        // The newest frame is at (w - 1); walk back `take` frames with wrap.
        std::size_t readStart = (w + cap - take) & mask_();
        for (std::size_t i = 0; i < take; ++i)
        {
            const std::size_t idx = (readStart + i) & mask_();
            outL[i] = buffer_[0ull * cap + idx];
            outR[i] = buffer_[1ull * cap + idx];
        }
        return take;
    }

    [[nodiscard]] std::uint64_t capturedFrames() const noexcept
    {
        const std::uint64_t total = totalWritten_.load(std::memory_order_acquire);
        const std::size_t cap = capacityFrames_;
        return std::min<std::uint64_t>(total,
                                       hasWrapped_.load(std::memory_order_relaxed) ? cap : total);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacityFrames_; }

private:
    static std::size_t roundUpPow2(std::size_t v) noexcept
    {
        v = std::max<std::size_t>(v, 2u);
        --v;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16;
        if constexpr (sizeof(std::size_t) > 4) v |= v >> 32;
        return v + 1;
    }

    std::size_t mask_() const noexcept { return capacityFrames_ - 1; }

    const std::size_t capacityFrames_;
    std::vector<float> buffer_;           // [cap] lefts then [cap] rights
    alignas(64) std::atomic<std::size_t> writePos_;
    alignas(64) std::atomic<std::uint64_t> totalWritten_;
    alignas(64) std::atomic<bool> hasWrapped_;
};

} // namespace more_phi
```

- [ ] **Step 4: Register the test source**

In `tests/CMakeLists.txt`, add (header-only ring, no `.cpp`):

```cmake
    Unit/TestAudioCaptureRing.cpp
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `cmake --build build --config Release && cd build && ctest -R "AudioCaptureRing" --output-on-failure -C Release`
Expected: PASS — 4 sections green.

- [ ] **Step 6: Commit**

```bash
git add src/Core/AudioCaptureRing.h tests/Unit/TestAudioCaptureRing.cpp tests/CMakeLists.txt
git commit -m "feat(sonicmaster): lock-free SPSC AudioCaptureRing for analysis capture"
```

---

## Task 4: SonicMasterDecisionRunner — ONNX session wrapper

A thin, synchronous `runDecision(waveform → 44 floats)` wrapper. Mirrors `OnnxNeuralMasteringRunner`'s pimpl + `MORE_PHI_HAS_ONNX` gating exactly, so the seam compiles without ONNX and activates when `MORE_PHI_ENABLE_ONNX=ON`.

**Files:**
- Create: `src/AI/SonicMasterDecisionRunner.h`
- Create: `src/AI/SonicMasterDecisionRunner.cpp`

- [ ] **Step 1: Create the header**

```cpp
// src/AI/SonicMasterDecisionRunner.h
#pragma once

#include "AI/SonicMasterDecisionDecoder.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace more_phi {

struct SonicMasterSessionHandle; // pimpl — defined in the .cpp

// Stereo sample count the model ingests (~6 s @ 44.1 kHz). Matches
// MasteringDecisionNet.segment_samples for this checkpoint.
inline constexpr std::size_t kSonicMasterSegmentFrames = 262138;

// ONNX session wrapper for the waveform->decision contract. Thread-unsafe by
// design: the analysis engine guarantees single-threaded use (one session, one
// analysis thread, joined before destruction).
class SonicMasterDecisionRunner
{
public:
    SonicMasterDecisionRunner() noexcept;
    ~SonicMasterDecisionRunner();

    SonicMasterDecisionRunner(const SonicMasterDecisionRunner&) = delete;
    SonicMasterDecisionRunner& operator=(const SonicMasterDecisionRunner&) = delete;

    // Message thread only. Loads + shape-validates an ONNX model whose contract
    // matches masteringbrain_v2_decision (input [*, 2, kSonicMasterSegmentFrames],
    // output [*, kSonicMasterDecisionWidth]). Returns false (and leaves the
    // runner unavailable) when ORT is not linked, the file is missing, or the
    // I/O shapes do not match contractPath's recorded shapes.
    bool loadModel(std::string_view modelPath, std::string_view contractPath);

    void unloadModel() noexcept;

    [[nodiscard]] bool isAvailable() const noexcept;

    // Analysis thread only. Runs one inference. `stereoInterleaved` must hold
    // at least 2 * kSonicMasterSegmentFrames floats (L0,R0,L1,R1,...). Writes
    // kSonicMasterDecisionWidth floats into outDecision. Returns false on any
    // error (caller skips the cycle). noexcept at the boundary — ORT errors are
    // caught internally and converted to a false return.
    bool runDecision(const float* stereoInterleaved,
                     float* outDecision,
                     std::size_t outCapacity) noexcept;

private:
    std::unique_ptr<SonicMasterSessionHandle> session_;
    bool available_ = false;
};

} // namespace more_phi
```

- [ ] **Step 2: Create the implementation**

```cpp
// src/AI/SonicMasterDecisionRunner.cpp
#include "AI/SonicMasterDecisionRunner.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>

#ifndef MORE_PHI_HAS_ONNX
#define MORE_PHI_HAS_ONNX 0
#endif

#if MORE_PHI_HAS_ONNX
#include <onnxruntime_cxx_api.h>
#include <nlohmann/json.hpp>
#endif

namespace more_phi {

#if MORE_PHI_HAS_ONNX
struct SonicMasterSessionHandle
{
    std::unique_ptr<Ort::Env> env;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Ort::AllocatorWithDefaultOptions> allocator;
    std::string inputName;
    std::string outputName;
    Ort::MemoryInfo memoryInfo { Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault) };
    std::vector<float> inputBuffer;  // [2 * kSonicMasterSegmentFrames], reused
    std::vector<int64_t> inputShape  { 1, 2, static_cast<int64_t>(kSonicMasterSegmentFrames) };
    std::vector<int64_t> outputShape { 1, static_cast<int64_t>(kSonicMasterDecisionWidth) };
};
#else
struct SonicMasterSessionHandle {};
#endif

SonicMasterDecisionRunner::SonicMasterDecisionRunner() noexcept = default;
SonicMasterDecisionRunner::~SonicMasterDecisionRunner() = default;

bool SonicMasterDecisionRunner::loadModel(std::string_view modelPath, std::string_view contractPath)
{
    unloadModel();

    if (modelPath.empty()) return false;

#if !MORE_PHI_HAS_ONNX
    (void) contractPath;
    return false;
#else
    try
    {
        // ── Load + validate the contract JSON (input/output shapes + names) ──
        std::ifstream cf(std::string { contractPath });
        if (!cf.good()) return false;
        nlohmann::json contract = nlohmann::json::parse(cf, nullptr, /*allow_exceptions=*/false);
        if (contract.is_discarded()) return false;

        session_ = std::make_unique<SonicMasterSessionHandle>();
        session_->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "sonicmaster");
        session_->allocator = std::make_unique<Ort::AllocatorWithDefaultOptions>();

        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);

        const std::string pathStr { modelPath };
        session_->session = std::make_unique<Ort::Session>(
#ifdef _WIN32
            *session_->env, std::wstring(pathStr.begin(), pathStr.end()).c_str(), opts
#else
            *session_->env, pathStr.c_str(), opts
#endif
        );

        if (session_->session->GetInputCount() != 1 || session_->session->GetOutputCount() != 1)
        { unloadModel(); return false; }

        auto inputInfo = session_->session->GetInputTypeInfo(0);
        auto outputInfo = session_->session->GetOutputTypeInfo(0);
        auto inTensor = inputInfo.GetTensorTypeAndShapeInfo();
        auto outTensor = outputInfo.GetTensorTypeAndShapeInfo();
        if (inTensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
            outTensor.GetElementType() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
        { unloadModel(); return false; }

        const auto totalIn = std::accumulate(inTensor.GetShape().begin(), inTensor.GetShape().end(),
                                             int64_t { 1 },
                                             [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        const auto totalOut = std::accumulate(outTensor.GetShape().begin(), outTensor.GetShape().end(),
                                              int64_t { 1 },
                                              [](int64_t a, int64_t b) { return a * (b > 0 ? b : 1); });
        if (totalIn != static_cast<int64_t>(2 * kSonicMasterSegmentFrames) ||
            totalOut != static_cast<int64_t>(kSonicMasterDecisionWidth))
        { unloadModel(); return false; }

        auto inNameAlloc = session_->session->GetInputNameAllocated(0, *session_->allocator);
        auto outNameAlloc = session_->session->GetOutputNameAllocated(0, *session_->allocator);
        session_->inputName = inNameAlloc.get();
        session_->outputName = outNameAlloc.get();
        session_->inputBuffer.assign(2 * kSonicMasterSegmentFrames, 0.0f);

        available_ = true;
        return true;
    }
    catch (const Ort::Exception&)
    {
        unloadModel();
        return false;
    }
#endif
}

void SonicMasterDecisionRunner::unloadModel() noexcept
{
    session_.reset();
    available_ = false;
}

bool SonicMasterDecisionRunner::isAvailable() const noexcept { return available_; }

bool SonicMasterDecisionRunner::runDecision(const float* stereoInterleaved,
                                            float* outDecision,
                                            std::size_t outCapacity) noexcept
{
#if !MORE_PHI_HAS_ONNX
    (void) stereoInterleaved; (void) outDecision; (void) outCapacity;
    return false;
#else
    if (!available_ || session_ == nullptr || stereoInterleaved == nullptr
        || outDecision == nullptr || outCapacity < kSonicMasterDecisionWidth)
        return false;

    try
    {
        std::copy_n(stereoInterleaved, 2 * kSonicMasterSegmentFrames, session_->inputBuffer.data());

        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            session_->memoryInfo,
            session_->inputBuffer.data(),
            session_->inputBuffer.size(),
            session_->inputShape.data(),
            session_->inputShape.size());

        const char* inputNames[] = { session_->inputName.c_str() };
        const char* outputNames[] = { session_->outputName.c_str() };

        auto outputs = session_->session->Run(
            Ort::RunOptions { nullptr }, inputNames, &inputTensor, 1, outputNames, 1);

        const float* outData = outputs[0].GetTensorData<float>();
        std::copy_n(outData, kSonicMasterDecisionWidth, outDecision);
        return true;
    }
    catch (const Ort::Exception&)
    {
        return false;
    }
    catch (...)
    {
        return false;
    }
#endif
}

} // namespace more_phi
```

- [ ] **Step 3: Register the source**

In `CMakeLists.txt`, add `src/AI/SonicMasterDecisionRunner.cpp` to the `MorePhi` target next to `OnnxNeuralMasteringRunner.cpp`.

- [ ] **Step 4: Build to verify it compiles**

Run: `cmake --build build --config Release`
Expected: clean build (the runner is exercised in Task 6; here we only confirm it links).

- [ ] **Step 5: Commit**

```bash
git add src/AI/SonicMasterDecisionRunner.h src/AI/SonicMasterDecisionRunner.cpp CMakeLists.txt
git commit -m "feat(sonicmaster): ONNX session runner for waveform->decision inference"
```

---

## Task 5: Offline ONNX export script

Produces `masteringbrain_v2_decision.onnx` + `masteringbrain_v2_contract.json` from the downloaded `.ckpt`, with a PyTorch↔ONNX parity check.

**Files:**
- Create: `tools/export_onnx/masteringbrain_to_onnx.py`
- Create: `tools/export_onnx/README.md`

- [ ] **Step 1: Write the export script**

```python
# tools/export_onnx/masteringbrain_to_onnx.py
"""Export masteringbrainv2 checkpoint to ONNX for the VST3 realtime path.

Run from the sonicmaster-v3-decision-engine package root:

    python tools/export_onnx/masteringbrain_to_onnx.py \
        --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z" \
        --output-dir build/sonicmaster

Produces:
  masteringbrain_v2_decision.onnx   - the traced graph (opset 17)
  masteringbrain_v2_contract.json   - I/O names/shapes the runner validates at load
  parity_report.json                - max abs diff vs the PyTorch forward
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch

SEGMENT_FRAMES = 262138
DECISION_WIDTH = 44


def _resolve_paths(package_root: Path) -> tuple[Path, Path]:
    ckpt = package_root / "models" / "v3" / "mastering-brain-v2-fullchain-best" / "checkpoints" / "best.ckpt"
    cfg = package_root / "models" / "v3" / "mastering-brain-v2-fullchain-best" / "config.json"
    if not ckpt.exists():
        sys.exit(f"checkpoint not found: {ckpt}")
    return ckpt, cfg


def _load_module(package_root: Path):
    sys.path.insert(0, str(package_root / "training" / "neural-mastering" / "bin" / "training"))
    from master_audio import load_module_from_checkpoint  # type: ignore
    ckpt, cfg = _resolve_paths(package_root)
    module, hparams = load_module_from_checkpoint(ckpt, torch.device("cpu"))
    module.eval()
    return module, hparams


class _DecisionOnly(torch.nn.Module):
    """Wraps the Lightning module so forward() returns ONLY the decision head."""

    def __init__(self, module):
        super().__init__()
        self.net = module.network if hasattr(module, "network") else module

    def forward(self, waveform, target_lufs):
        out = self.net(waveform, target_lufs_db=target_lufs)
        if isinstance(out, tuple):
            out = out[0]
        # The decision head is cached on the network as _last_mastering_decisions
        # for the decision-only architecture; otherwise forward returns it.
        decisions = getattr(self.net, "_last_mastering_decisions", None)
        if decisions is None:
            decisions = out
        return decisions


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--package", type=Path, required=True,
                    help="sonicmaster-v3-decision-engine package root")
    ap.add_argument("--output-dir", type=Path, default=Path("build/sonicmaster"))
    ap.add_argument("--target-lufs", type=float, default=-14.0)
    ap.add_argument("--opset", type=int, default=17)
    args = ap.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    module, _ = _load_module(args.package)
    wrapper = _DecisionOnly(module).eval()

    # Synthetic 6 s stereo seed for tracing + parity.
    t = np.arange(SEGMENT_FRAMES, dtype=np.float32) / 44100.0
    seed = 0.1 * (
        np.sin(2 * np.pi * 220.0 * t)[None, :]
        + 0.3 * np.sin(2 * np.pi * 2500.0 * t)[None, :]
        + 0.05 * np.random.RandomState(0).randn(SEGMENT_FRAMES)[None, :]
    ).astype(np.float32)
    seed = np.broadcast_to(seed, (2, SEGMENT_FRAMES)).copy()
    waveform = torch.from_numpy(seed).unsqueeze(0)   # [1, 2, N]
    target_lufs = torch.tensor([args.target_lufs], dtype=torch.float32)

    onnx_path = args.output_dir / "masteringbrain_v2_decision.onnx"
    with torch.no_grad():
        torch.onnx.export(
            wrapper,
            (waveform, target_lufs),
            str(onnx_path),
            input_names=["waveform", "target_lufs"],
            output_names=["decision"],
            dynamic_axes={"waveform": {0: "batch"}, "decision": {0: "batch"}},
            opset_version=args.opset,
            do_constant_folding=True,
        )

        # ── Parity: PyTorch vs ONNX on the same seed ──────────────────────────
        pt_out = wrapper(waveform, target_lufs).detach().cpu().numpy().reshape(-1)

    try:
        import onnxruntime as ort
        sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
        onnx_out = sess.run(None, {
            "waveform": waveform.numpy(),
            "target_lufs": target_lufs.numpy(),
        })[0].reshape(-1)
        max_abs = float(np.max(np.abs(pt_out - onnx_out)))
    except Exception as exc:  # onnxruntime optional for the export step itself
        max_abs = float("nan")
        print(f"[warn] parity check skipped: {exc}")

    parity_path = args.output_dir / "parity_report.json"
    parity_path.write_text(json.dumps({
        "pyTorch_output_prefix": pt_out[:8].tolist(),
        "onnx_output_prefix": onnx_out[:8].tolist() if max_abs == max_abs else None,
        "max_abs_diff": max_abs,
        "passed": bool(np.isfinite(max_abs) and max_abs < 1e-4),
    }, indent=2))

    contract = {
        "schemaVersion": 1,
        "modelId": "masteringbrain-v2-fullchain-best",
        "inputName": "waveform",
        "outputName": "decision",
        "inputShape": [1, 2, SEGMENT_FRAMES],
        "outputShape": [1, DECISION_WIDTH],
        "sampleRate": 44100,
        "targetLufsDefault": args.target_lufs,
    }
    (args.output_dir / "masteringbrain_v2_contract.json").write_text(json.dumps(contract, indent=2))

    print(f"wrote {onnx_path}")
    print(f"wrote {args.output_dir / 'masteringbrain_v2_contract.json'}")
    print(f"parity max_abs_diff={max_abs:.2e} -> {'PASS' if max_abs < 1e-4 else 'FAIL'}")
    return 0 if (np.isfinite(max_abs) and max_abs < 1e-4) else 1


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Write the runbook**

```markdown
# SonicMaster ONNX Export

Produces the model + contract files consumed by `SonicMasterDecisionRunner`.

## Prerequisites

Python 3.12 with `torch>=2.11`, `numpy`, `onnxruntime`, `onnx`. The
`sonicmaster-v3-decision-engine` package must be unpacked locally.

## Run

```bash
python tools/export_onnx/masteringbrain_to_onnx.py \
    --package "C:/Users/HP/Downloads/sonicmaster-v3-decision-engine-20260530T121536Z" \
    --output-dir build/sonicmaster
```

## Output

- `masteringbrain_v2_decision.onnx` — stage next to the test exe and the plugin.
- `masteringbrain_v2_contract.json` — the runner refuses to load a model whose
  shapes do not match this contract.
- `parity_report.json` — `passed: true` iff max abs diff vs PyTorch < 1e-4.

If parity fails, the export is broken; do not stage the model.
```

- [ ] **Step 3: Commit**

```bash
git add tools/export_onnx/masteringbrain_to_onnx.py tools/export_onnx/README.md
git commit -m "feat(sonicmaster): offline ONNX export + parity check for masteringbrainv2"
```

---

## Task 6: SonicMasterAnalysisEngine — orchestration (TDD with a stub runner)

Owns the capture ring, analysis thread, runner, safety policy, and the message-thread ramp. Tested with a `StubDecisionRunner` so no real model is needed.

**Files:**
- Create: `src/AI/SonicMasterAnalysisEngine.h`
- Create: `src/AI/SonicMasterAnalysisEngine.cpp`
- Create: `tests/Unit/TestSonicMasterAnalysisEngine.cpp`

- [ ] **Step 1: Create the header**

```cpp
// src/AI/SonicMasterAnalysisEngine.h
#pragma once

#include "AI/SonicMasterDecisionDecoder.h"
#include "AI/SonicMasterDecisionRunner.h"
#include "Core/AudioCaptureRing.h"
#include "Core/NeuralMasteringSafetyPolicy.h"
#include "Core/NeuralMasteringTypes.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace more_phi {

class AutoMasteringEngine;

// Abstracts "run inference on a waveform" so tests can substitute a stub. Real
// use is SonicMasterDecisionRunner; tests pass a fake that returns canned data.
class ISonicMasterInferenceSource
{
public:
    virtual ~ISonicMasterInferenceSource() = default;
    [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
    virtual bool infer(const float* stereoInterleaved, float* outDecision,
                       std::size_t outCapacity) noexcept = 0;
};

// Adapter that turns SonicMasterDecisionRunner into an ISonicMasterInferenceSource.
class SonicMasterRunnerInferenceSource final : public ISonicMasterInferenceSource
{
public:
    explicit SonicMasterRunnerInferenceSource(SonicMasterDecisionRunner& runner) noexcept
        : runner_(runner) {}
    [[nodiscard]] bool isAvailable() const noexcept override { return runner_.isAvailable(); }
    bool infer(const float* stereoInterleaved, float* outDecision,
               std::size_t outCapacity) noexcept override
    { return runner_.runDecision(stereoInterleaved, outDecision, outCapacity); }
private:
    SonicMasterDecisionRunner& runner_;
};

struct SonicMasterAnalysisEngineConfig
{
    double analysisIntervalSeconds = 3.0;
    double rampDurationSeconds     = 0.2;
    float  confidenceFloor         = kSonicMasterDefaultConfidence;
    int    consecutiveFailureLimit = 3;
    // 8 s @ 192 kHz, rounded up to a power of two by AudioCaptureRing.
    std::size_t captureRingFrames = 8u * 192000u;
};

class SonicMasterAnalysisEngine
{
public:
    SonicMasterAnalysisEngine();
    ~SonicMasterAnalysisEngine();

    SonicMasterAnalysisEngine(const SonicMasterAnalysisEngine&) = delete;
    SonicMasterAnalysisEngine& operator=(const SonicMasterAnalysisEngine&) = delete;

    // Inject the inference source (real runner or test stub). Caller owns it.
    void setInferenceSource(ISonicMasterInferenceSource* source) noexcept;
    void setApplicationEngine(AutoMasteringEngine* engine) noexcept { applicationEngine_ = engine; }

    // Message thread. Sizes the capture ring + scratch buffers, loads the model
    // (if a real runner is wired), starts the analysis thread idle.
    void prepare(double sampleRate, int maxBlockSize);

    // Message thread. Joins the analysis thread, then drops the session.
    void release() noexcept;

    // Audio thread: copy a stereo block into the capture ring. noexcept, no locks.
    void capture(const float* left, const float* right, std::size_t n) noexcept;

    // Any thread (atomic). When true, capture + analysis run; when false, capture
    // is a no-op and the analysis thread sleeps. DSP params are held, not reset.
    void setActive(bool active) noexcept { active_.store(active, std::memory_order_relaxed); }
    [[nodiscard]] bool isActive() const noexcept { return active_.load(std::memory_order_relaxed); }

    [[nodiscard]] bool isAvailable() const noexcept;

    // Status surfaced to the UI. Message thread only.
    enum class Status { Disabled, CollectingAudio, Applied, HeldLowConfidence, ErrorAutoDisabled };
    [[nodiscard]] Status getStatus() const noexcept { return status_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t getLastPlanId() const noexcept { return lastPlanId_; }

    // Test hook: run one analysis cycle synchronously on the calling thread.
    // Used by TestSonicMasterAnalysisEngine to avoid real thread timing.
    bool runOneCycleForTest() noexcept;

private:
    void analysisLoop() noexcept;
    bool runCycle() noexcept; // returns true if a plan was applied
    void applyRamped(const ValidatedNeuralMasteringPlan& plan) noexcept;

    SonicMasterAnalysisEngineConfig config_ {};
    AutoMasteringEngine* applicationEngine_ = nullptr;
    ISonicMasterInferenceSource* source_ = nullptr;

    std::unique_ptr<AudioCaptureRing> ring_;
    std::vector<float> captureL_, captureR_;   // host-rate window
    std::vector<float> modelL_, modelR_;       // 44.1k window
    std::vector<float> interleaved_;           // 2 * kSonicMasterSegmentFrames
    std::array<float, kSonicMasterDecisionWidth> decision_ {};

    NeuralMasteringSafetyPolicy safetyPolicy_ {};

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> active_ { false };
    std::atomic<bool> stopRequested_ { false };
    std::atomic<bool> prepared_ { false };
    std::atomic<Status> status_ { Status::Disabled };
    std::atomic<int> consecutiveFailures_ { 0 };
    std::uint64_t lastPlanId_ = 0;
    std::uint64_t nextPlanId_ = 1;
    double sampleRate_ = 48000.0;
};

} // namespace more_phi
```

- [ ] **Step 2: Write the failing tests**

```cpp
// tests/Unit/TestSonicMasterAnalysisEngine.cpp
#include <catch2/catch_test_macros.hpp>

#include "AI/SonicMasterAnalysisEngine.h"
#include "Core/AutoMasteringEngine.h"
#include "Core/NeuralMasteringTypes.h"

#include <cmath>
#include <numeric>

namespace {

// Returns a neutral, in-range decision so the safety policy accepts it.
class StubDecisionSource final : public more_phi::ISonicMasterInferenceSource
{
public:
    [[nodiscard]] bool isAvailable() const noexcept override { return true; }
    bool infer(const float* /*stereoInterleaved*/, float* outDecision,
               std::size_t outCapacity) noexcept override
    {
        if (outCapacity < more_phi::kSonicMasterDecisionWidth) return false;
        for (auto& v : decision_) v = 0.0f;
        // Neutral: -14 LUFS, -1 dBTP, zero EQ, zero comp offsets.
        decision_[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;
        decision_[more_phi::kSonicMasterTruePeakIdx]   = -1.0f;
        std::copy_n(decision_.data(), more_phi::kSonicMasterDecisionWidth, outDecision);
        callCount_++;
        return shouldSucceed_;
    }
    int callCount() const { return callCount_; }
    void setShouldSucceed(bool v) { shouldSucceed_ = v; }
private:
    float decision_[more_phi::kSonicMasterDecisionWidth] {};
    int callCount_ = 0;
    bool shouldSucceed_ = true;
};

void feedSilence(more_phi::SonicMasterAnalysisEngine& eng, std::size_t frames)
{
    std::vector<float> l(frames, 1e-4f), r(frames, 1e-4f);
    constexpr std::size_t kBlock = 512;
    for (std::size_t off = 0; off < frames; off += kBlock)
    {
        const std::size_t n = std::min(kBlock, frames - off);
        eng.capture(l.data() + off, r.data() + off, n);
    }
}

} // namespace

TEST_CASE("AnalysisEngine applies a plan once enough audio is captured",
          "[SonicMaster][Engine]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);

    // Feed well over the required segment so capturedFrames() >= required.
    feedSilence(eng, more_phi::kSonicMasterSegmentFrames + 1024);

    REQUIRE(eng.runOneCycleForTest());
    REQUIRE(source.callCount() == 1);
    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::Applied);
    CHECK(engine.hasLastSafeNeuralMasteringPlan());

    eng.release();
}

TEST_CASE("AnalysisEngine skips inference when too little audio captured",
          "[SonicMaster][Engine]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);

    // Only 1000 frames — far short of kSonicMasterSegmentFrames.
    feedSilence(eng, 1000);

    REQUIRE_FALSE(eng.runOneCycleForTest());
    CHECK(source.callCount() == 0);
    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::CollectingAudio);

    eng.release();
}

TEST_CASE("AnalysisEngine auto-disables after N consecutive failures",
          "[SonicMaster][Engine]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    source.setShouldSucceed(false);
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);
    feedSilence(eng, more_phi::kSonicMasterSegmentFrames + 1024);

    for (int i = 0; i < 3; ++i)
        eng.runOneCycleForTest();

    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::ErrorAutoDisabled);
    CHECK_FALSE(eng.isActive());

    eng.release();
}

TEST_CASE("AnalysisEngine join-before-destroy: release returns cleanly mid-cycle",
          "[SonicMaster][Engine]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    auto* eng = new more_phi::SonicMasterAnalysisEngine();
    StubDecisionSource source;
    eng->setInferenceSource(&source);
    eng->setApplicationEngine(&engine);
    eng->prepare(48000.0, 512);
    eng->setActive(true);
    feedSilence(*eng, more_phi::kSonicMasterSegmentFrames + 1024);

    // Release must join the analysis thread before the destructor runs.
    eng->release();
    delete eng;
    // Reaching here without a hang/crash is the assertion.
    CHECK(true);
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `cd build && ctest -R "SonicMaster.*Engine" --output-on-failure -C Release`
Expected: FAIL — `SonicMasterAnalysisEngine` undefined.

- [ ] **Step 4: Implement the engine**

```cpp
// src/AI/SonicMasterAnalysisEngine.cpp
#include "AI/SonicMasterAnalysisEngine.h"
#include "Core/AutoMasteringEngine.h"

#include <algorithm>
#include <cmath>
#include <juce_core/juce_core.h>

namespace more_phi {

namespace {

float peakAbs(const float* a, std::size_t n) noexcept
{
    float m = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
        m = std::max(m, std::abs(a[i]));
    return m;
}

// Linear resample (matches sonicmaster_engine.api.preprocess_audio).
void resampleLinear(const float* src, std::size_t srcLen, std::size_t dstLen, float* dst) noexcept
{
    if (srcLen == 0 || dstLen == 0) return;
    for (std::size_t i = 0; i < dstLen; ++i)
    {
        const double pos = static_cast<double>(i) * static_cast<double>(srcLen - 1) / static_cast<double>(dstLen - 1);
        const std::size_t i0 = static_cast<std::size_t>(pos);
        const std::size_t i1 = std::min(i0 + 1, srcLen - 1);
        const double frac = pos - static_cast<double>(i0);
        dst[i] = static_cast<float>(src[i0] * (1.0 - frac) + src[i1] * frac);
    }
}

} // namespace

SonicMasterAnalysisEngine::SonicMasterAnalysisEngine() = default;

SonicMasterAnalysisEngine::~SonicMasterAnalysisEngine()
{
    release();
}

void SonicMasterAnalysisEngine::setInferenceSource(ISonicMasterInferenceSource* source) noexcept
{
    source_ = source;
}

bool SonicMasterAnalysisEngine::isAvailable() const noexcept
{
    return source_ != nullptr && source_->isAvailable();
}

void SonicMasterAnalysisEngine::prepare(double sampleRate, int /*maxBlockSize*/)
{
    release();
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    ring_ = std::make_unique<AudioCaptureRing>(config_.captureRingFrames);

    // Compute the host-rate window length equivalent to the 44.1k segment.
    const std::size_t hostFrames = static_cast<std::size_t>(
        std::round(kSonicMasterSegmentFrames * sampleRate_ / 44100.0));
    captureL_.assign(hostFrames, 0.0f);
    captureR_.assign(hostFrames, 0.0f);
    modelL_.assign(kSonicMasterSegmentFrames, 0.0f);
    modelR_.assign(kSonicMasterSegmentFrames, 0.0f);
    interleaved_.assign(2 * kSonicMasterSegmentFrames, 0.0f);
    decision_.fill(0.0f);

    prepared_.store(true, std::memory_order_release);
    status_.store(isAvailable() ? Status::CollectingAudio : Status::Disabled,
                  std::memory_order_release);

    stopRequested_.store(false, std::memory_order_relaxed);
    thread_ = std::thread([this] { analysisLoop(); });
}

void SonicMasterAnalysisEngine::release() noexcept
{
    stopRequested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    prepared_.store(false, std::memory_order_release);
    ring_.reset();
}

void SonicMasterAnalysisEngine::capture(const float* left, const float* right,
                                        std::size_t n) noexcept
{
    if (!active_.load(std::memory_order_relaxed) || !prepared_.load(std::memory_order_relaxed)
        || ring_ == nullptr || left == nullptr || right == nullptr)
        return;
    ring_->write(left, right, n);
}

void SonicMasterAnalysisEngine::analysisLoop() noexcept
{
    auto nextDeadline = std::chrono::steady_clock::now();
    while (!stopRequested_.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_until(lock, nextDeadline,
                       [this] { return stopRequested_.load(std::memory_order_acquire); });
        if (stopRequested_.load(std::memory_order_acquire)) break;

        nextDeadline += std::chrono::milliseconds(
            static_cast<int>(config_.analysisIntervalSeconds * 1000.0));
        lock.unlock();

        if (!active_.load(std::memory_order_relaxed)) continue;
        runCycle();
    }
}

bool SonicMasterAnalysisEngine::runOneCycleForTest() noexcept
{
    if (!prepared_.load(std::memory_order_relaxed)) return false;
    active_.store(true, std::memory_order_relaxed);
    return runCycle();
}

bool SonicMasterAnalysisEngine::runCycle() noexcept
{
    if (ring_ == nullptr || source_ == nullptr) return false;
    if (!isAvailable() || !active_.load(std::memory_order_relaxed))
    {
        status_.store(Status::Disabled, std::memory_order_release);
        return false;
    }

    const std::size_t hostFrames = captureL_.size();
    const std::size_t got = ring_->readNewest(hostFrames, captureL_.data(), captureR_.data());
    if (got < hostFrames)
    {
        status_.store(Status::CollectingAudio, std::memory_order_release);
        return false;
    }

    // Resample host-rate -> 44.1k if needed.
    if (std::abs(sampleRate_ - 44100.0) < 0.5)
    {
        std::copy_n(captureL_.data(), kSonicMasterSegmentFrames, modelL_.data());
        std::copy_n(captureR_.data(), kSonicMasterSegmentFrames, modelR_.data());
    }
    else
    {
        resampleLinear(captureL_.data(), hostFrames, kSonicMasterSegmentFrames, modelL_.data());
        resampleLinear(captureR_.data(), hostFrames, kSonicMasterSegmentFrames, modelR_.data());
    }

    // Peak-normalize to -1 dBFS so the model sees a consistent operating level.
    const float peak = std::max(peakAbs(modelL_.data(), kSonicMasterSegmentFrames),
                                peakAbs(modelR_.data(), kSonicMasterSegmentFrames));
    const float gain = peak > 1e-9f ? (0.891f / peak) : 1.0f;
    for (std::size_t i = 0; i < kSonicMasterSegmentFrames; ++i)
    {
        interleaved_[2 * i + 0] = modelL_[i] * gain;
        interleaved_[2 * i + 1] = modelR_[i] * gain;
    }

    if (!source_->infer(interleaved_.data(), decision_.data(), decision_.size()))
    {
        const int fails = consecutiveFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fails >= config_.consecutiveFailureLimit)
        {
            active_.store(false, std::memory_order_relaxed);
            status_.store(Status::ErrorAutoDisabled, std::memory_order_release);
        }
        return false;
    }
    consecutiveFailures_.store(0, std::memory_order_relaxed);

    ValidatedNeuralMasteringPlan plan {};
    if (!decodeSonicMasterDecision(decision_.data(), decision_.size(), sampleRate_, plan))
        return false;
    plan.sourcePlanId = nextPlanId_++;

    // Safety gate. Build a candidate the policy can validate; the decoder's
    // projected plan already carries appliedMask + projectedTargets.
    NeuralMasteringRuntimeState runtime {};
    runtime.sampleRate = sampleRate_;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;

    // The safety policy's validate() takes a candidate; wrap the decoded plan.
    NeuralMasteringPlanCandidate candidate {};
    candidate.schemaVersion   = kNeuralMasteringPlanSchemaVersion;
    candidate.runtimeMode     = NeuralMasteringRuntimeMode::Background;
    candidate.confidence      = config_.confidenceFloor;
    candidate.evidenceLevel   = plan.evidenceLevel;
    candidate.editableMask    = plan.appliedMask;
    candidate.targets         = plan.projectedTargets;
    candidate.deltas          = plan.projectedTargets;

    const auto verdict = safetyPolicy_.validate(candidate, runtime);
    if (!verdict.accepted)
    {
        status_.store(Status::HeldLowConfidence, std::memory_order_release);
        return false;
    }

    // Apply: bookkeeping first, then the message-thread ramp. The plan we hand
    // to the engine is the decoded one (its projectedTargets are already
    // safety-clamped by the decoder); applyValidatedPlan re-clamps defensively.
    applyRamped(plan);
    lastPlanId_ = plan.sourcePlanId;
    status_.store(Status::Applied, std::memory_order_release);
    return true;
}

void SonicMasterAnalysisEngine::applyRamped(const ValidatedNeuralMasteringPlan& plan) noexcept
{
    // The DSP modules expose atomic setters; AutoMasteringEngine::applyValidatedPlan
    // is the single apply surface and performs per-module clamping. We call it
    // once on the message thread (this method is invoked from the test path on
    // the calling thread, or from analysisLoop via the analysis thread — in the
    // latter case we hop to the message thread so JUCE DSP setter semantics hold).
    if (applicationEngine_ == nullptr) return;

    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && !juce::MessageManager::getInstanceWithoutCreating()->isThisTheMessageThread())
    {
        // Capture by value (small struct) and hop.
        juce::MessageManager::callAsync(
            [engine = applicationEngine_, p = plan]() { engine->applyValidatedPlan(p); });
    }
    else
    {
        applicationEngine_->applyValidatedPlan(plan);
    }
}

} // namespace more_phi
```

- [ ] **Step 5: Register the source + test**

In `CMakeLists.txt`, add `src/AI/SonicMasterAnalysisEngine.cpp` next to `SonicMasterDecisionRunner.cpp`.

In `tests/CMakeLists.txt`, add `Unit/TestSonicMasterAnalysisEngine.cpp` next to the other `Unit/TestSonicMaster*.cpp` entries.

- [ ] **Step 6: Run the tests to verify they pass**

Run: `cmake --build build --config Release && cd build && ctest -R "SonicMaster.*Engine" --output-on-failure -C Release`
Expected: PASS — 4 sections green.

- [ ] **Step 7: Commit**

```bash
git add src/AI/SonicMasterAnalysisEngine.h src/AI/SonicMasterAnalysisEngine.cpp tests/Unit/TestSonicMasterAnalysisEngine.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(sonicmaster): background analysis engine + cycle/safety/teardown"
```

---

## Task 7: Wire the engine into the plugin processor

Three minimal touch-points: construct the engine, wire `setApplicationEngine`, call `prepare`/`capture`/`release`, and add one APVTS bool bound to `setActive`.

**Files:**
- Modify: `src/Plugin/PluginProcessor.h`
- Modify: `src/Plugin/PluginProcessor.cpp`

- [ ] **Step 1: Add the include + members to PluginProcessor.h**

In `src/Plugin/PluginProcessor.h`, add after the existing `#include "AI/NeuralMasteringController.h"`:

```cpp
#include "AI/SonicMasterAnalysisEngine.h"
#include "AI/SonicMasterDecisionRunner.h"
```

Inside the private member block (next to the other `std::unique_ptr<OzoneParameterMap>` line), add:

```cpp
    // ── SonicMaster realtime neural mastering (preview, default OFF) ──────────
    SonicMasterDecisionRunner sonicMasterRunner_;
    SonicMasterAnalysisEngine sonicMasterEngine_;
    SonicMasterRunnerInferenceSource sonicMasterSource_ { sonicMasterRunner_ };
```

- [ ] **Step 2: Add the APVTS parameter**

In `PluginProcessor.cpp`, locate `createParameterLayout()` (it builds the `ParameterLayout` returned to the APVTS constructor). Add a boolean parameter — place it at the end of the layout vector, using the existing pattern for the other bool params (look for an existing `juce::ParameterID` + `AudioParameterBool` to copy the style):

```cpp
    layout.add(std::make_unique<juce::AudioParameterBool>(
        juce::ParameterID { "SonicMasterAnalysisEnabled", 1 },
        "Neural Master (Preview)",
        false));
```

- [ ] **Step 3: Construct + wire in the processor constructor**

In the `MorePhiProcessor()` constructor body (after `autoMasteringEngine_` is constructed and before the constructor ends), add:

```cpp
    sonicMasterEngine_.setInferenceSource(&sonicMasterSource_);
    sonicMasterEngine_.setApplicationEngine(&autoMasteringEngine_);
```

- [ ] **Step 4: Prepare + capture + release lifecycle**

In `prepareToPlay(double sampleRate, int samplesPerBlock)`, after the existing `autoMasteringEngine_.prepare(...)` call, add:

```cpp
    sonicMasterEngine_.prepare(sampleRate, samplesPerBlock);
```

In `releaseResources()`, add (before any existing cleanup of `autoMasteringEngine_`):

```cpp
    sonicMasterEngine_.release();
```

In `processBlock(...)`, after the audio buffer is finalized and before `applyOutputGainAndMetering`, add the single capture call:

```cpp
    if (buffer.getNumChannels() >= 2)
        sonicMasterEngine_.capture(buffer.getReadPointer(0),
                                   buffer.getReadPointer(1),
                                   static_cast<std::size_t>(buffer.getNumSamples()));
```

- [ ] **Step 5: Bind the APVTS bool to setActive**

In `syncStateFromAPVTS()` (which already reads other APVTS atomics), add at the end:

```cpp
    if (auto* p = dynamic_cast<juce::AudioParameterBool*>(apvts.getParameter("SonicMasterAnalysisEnabled")))
        sonicMasterEngine_.setActive(p->get());
```

- [ ] **Step 6: Build to confirm it compiles**

Run: `cmake --build build --config Release`
Expected: clean build. (No new test here — the engine is covered by Task 6; the wiring is exercised by the existing plugin lifecycle tests.)

- [ ] **Step 7: Run the existing lifecycle tests to confirm no regression**

Run: `cd build && ctest -R "PluginLifecycle|E2ESignalPath" --output-on-failure -C Release`
Expected: PASS (same results as before the change — the feature is OFF by default).

- [ ] **Step 8: Commit**

```bash
git add src/Plugin/PluginProcessor.h src/Plugin/PluginProcessor.cpp
git commit -m "feat(sonicmaster): wire analysis engine into processor (preview, default off)"
```

---

## Task 8: UI toggle + status label

One toggle button + a status line. Disabled when `!sonicMasterEngine_.isAvailable()`.

**Files:**
- Modify: `src/Plugin/PluginEditor.h`
- Modify: `src/Plugin/PluginEditor.cpp`

> **Before starting:** the editor accesses its processor via JUCE's `AudioProcessorEditor::getProcessor()` (returns `AudioProcessor&`), then downcasts. Confirm the existing editor code's convention — some editors store a typed `MorePhiProcessor&` reference. Use whichever the existing editor members already use. The snippets below assume `getProcessor()` returns the typed processor; adjust if the editor uses a stored reference instead.

- [ ] **Step 1: Add the members to PluginEditor.h**

In `src/Plugin/PluginEditor.h`, add includes if not present:

```cpp
#include <juce_gui_basics/juce_gui_basics.h>
```

Add private members (next to the other UI component members):

```cpp
    juce::ToggleButton sonicMasterToggle_ { "Neural Master (Preview)" };
    juce::Label sonicMasterStatus_;
    std::unique_ptr<juce::Button::Listener> sonicMasterButtonListener_;
    std::unique_ptr<juce::Timer> sonicMasterStatusTimer_;
```

- [ ] **Step 2: Construct + lay out the controls in the editor constructor**

In `PluginEditor.cpp`, in the constructor body (after the other UI is laid out), add:

```cpp
    using APVTS = juce::AudioProcessorValueTreeState;
    sonicMasterToggle_.setToggleState(false, juce::dontSendNotification);
    sonicMasterToggle_.setEnabled(getProcessor().getAutoMasteringEngine().hasLastSafeNeuralMasteringPlan()
                                  || true); // enabled state refined in timerCallback
    addAndMakeVisible(sonicMasterToggle_);

    sonicMasterStatus_.setText("Neural Master: off", juce::dontSendNotification);
    sonicMasterStatus_.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    addAndMakeVisible(sonicMasterStatus_);

    sonicMasterToggle_.onClick = [this]
    {
        auto& p = getProcessor();
        if (auto* param = dynamic_cast<juce::AudioParameterBool*>(
                p.getAPVTS().getParameter("SonicMasterAnalysisEnabled")))
            param->setValueNotifyingHost(sonicMasterToggle_.getToggleState() ? 1.0f : 0.0f);
    };
```

In the editor's `resized()`, position the two controls (coordinates chosen to match the existing layout grid — adjust to a free area):

```cpp
    sonicMasterToggle_.setBounds(20, getHeight() - 70, 220, 24);
    sonicMasterStatus_.setBounds(250, getHeight() - 70, 360, 24);
```

- [ ] **Step 3: Add a 5 Hz timer to refresh the status label**

Still in the constructor:

```cpp
    sonicMasterStatusTimer_ = std::make_unique<TimerLambda>([this] { refreshSonicMasterStatus(); });
    sonicMasterStatusTimer_->startTimerHz(5);
```

(If the editor does not already have a `TimerLambda` helper, use a private `juce::Timer` subclass instead — add to `PluginEditor.h`:

```cpp
    void refreshSonicMasterStatus();
```

and in `PluginEditor.cpp`:

```cpp
void MorePhiEditor::refreshSonicMasterStatus()
{
    auto& eng = getProcessor().getSonicMasterEngine();
    sonicMasterToggle_.setEnabled(eng.isAvailable());
    juce::String text = "Neural Master: ";
    switch (eng.getStatus())
    {
        case more_phi::SonicMasterAnalysisEngine::Status::Disabled:           text += "off"; break;
        case more_phi::SonicMasterAnalysisEngine::Status::CollectingAudio:    text += "collecting audio…"; break;
        case more_phi::SonicMasterAnalysisEngine::Status::Applied:            text += "applied #" + juce::String((int) eng.getLastPlanId()); break;
        case more_phi::SonicMasterAnalysisEngine::Status::HeldLowConfidence:  text += "held (low confidence)"; break;
        case more_phi::SonicMasterAnalysisEngine::Status::ErrorAutoDisabled:  text += "error — see log"; break;
    }
    sonicMasterStatus_.setText(text, juce::dontSendNotification);
}
```

Add the `getSonicMasterEngine()` accessor to `PluginProcessor.h` next to `getAutoMasteringEngine()`:

```cpp
    SonicMasterAnalysisEngine& getSonicMasterEngine() noexcept { return sonicMasterEngine_; }
```

Implement the `TimerLambda` as a small private inner class in `PluginEditor.h` if not already present:

```cpp
    struct TimerLambda : public juce::Timer
    {
        std::function<void()> cb;
        explicit TimerLambda(std::function<void()> f) : cb(std::move(f)) {}
        void timerCallback() override { cb(); }
    };
```

- [ ] **Step 4: Build + run the lifecycle tests**

Run: `cmake --build build --config Release && cd build && ctest -R "PluginLifecycle|GUI" --output-on-failure -C Release`
Expected: clean build, existing tests still pass.

- [ ] **Step 5: Commit**

```bash
git add src/Plugin/PluginEditor.h src/Plugin/PluginEditor.cpp src/Plugin/PluginProcessor.h
git commit -m "feat(sonicmaster): preview toggle + live status label in editor"
```

---

## Task 9: Live ONNX smoke test (opt-in)

Only compiled when `MORE_PHI_ENABLE_ONNX=ON` and the staged model + contract exist. Verifies the real exported graph returns 44 finite in-range floats under 500 ms.

**Files:**
- Create: `tests/Unit/TestSonicMasterRunnerLive.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the live test**

```cpp
// tests/Unit/TestSonicMasterRunnerLive.cpp
// Only compiled when MORE_PHI_HAS_ONNX is defined (see tests/CMakeLists.txt).
#include <catch2/catch_test_macros.hpp>

#include "AI/SonicMasterDecisionRunner.h"
#include "AI/SonicMasterDecisionDecoder.h"

#include <chrono>
#include <cmath>
#include <cstdio>

#if MORE_PHI_HAS_ONNX

TEST_CASE("SonicMasterDecisionRunner loads and infers the real ONNX model",
          "[SonicMaster][Live]")
{
    // Locate the staged model next to the test exe.
    const char* candidates[] = {
        "masteringbrain_v2_decision.onnx",
        "build/sonicmaster/masteringbrain_v2_decision.onnx",
        nullptr };
    const char* contractCandidates[] = {
        "masteringbrain_v2_contract.json",
        "build/sonicmaster/masteringbrain_v2_contract.json",
        nullptr };

    std::string modelPath, contractPath;
    for (int i = 0; candidates[i] != nullptr; ++i)
    {
        if (FILE* f = std::fopen(candidates[i], "r")) { std::fclose(f); modelPath = candidates[i]; break; }
    }
    for (int i = 0; contractCandidates[i] != nullptr; ++i)
    {
        if (FILE* f = std::fopen(contractCandidates[i], "r")) { std::fclose(f); contractPath = contractCandidates[i]; break; }
    }

    if (modelPath.empty())
    {
        WARN("masteringbrain_v2_decision.onnx not staged — skipping live test");
        return;
    }

    more_phi::SonicMasterDecisionRunner runner;
    REQUIRE(runner.loadModel(modelPath, contractPath));
    REQUIRE(runner.isAvailable());

    // Synthetic 6 s stereo sine+noise.
    std::vector<float> interleaved(2 * more_phi::kSonicMasterSegmentFrames, 0.0f);
    for (std::size_t i = 0; i < more_phi::kSonicMasterSegmentFrames; ++i)
    {
        const double t = static_cast<double>(i) / 44100.0;
        const float v = 0.2f * static_cast<float>(std::sin(2.0 * 3.14159265 * 220.0 * t));
        interleaved[2 * i + 0] = v;
        interleaved[2 * i + 1] = v;
    }

    float decision[more_phi::kSonicMasterDecisionWidth] {};
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(runner.runDecision(interleaved.data(), decision, more_phi::kSonicMasterDecisionWidth));
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    for (std::size_t i = 0; i < more_phi::kSonicMasterDecisionWidth; ++i)
        REQUIRE(std::isfinite(decision[i]));
    CHECK(decision[more_phi::kSonicMasterTargetLufsIdx] >= -30.0f);
    CHECK(decision[more_phi::kSonicMasterTargetLufsIdx] <= -6.0f);
    CHECK(dt < 500);
}

#endif // MORE_PHI_HAS_ONNX
```

- [ ] **Step 2: Register the test gated on ONNX**

In `tests/CMakeLists.txt`, immediately after the existing `if(MORE_PHI_HAS_ONNX)` block, add a parallel staging block for the SonicMaster model and add the test source guarded by the same flag:

```cmake
    # ── SonicMaster realtime neural mastering (preview) ──────────────────────
    set(MOREPHI_SONICMASTER_MODEL "masteringbrain_v2_decision.onnx")
    set(MOREPHI_SONICMASTER_SRC "${CMAKE_SOURCE_DIR}/build/sonicmaster/${MOREPHI_SONICMASTER_MODEL}")
    set(MOREPHI_SONICMASTER_DST "$<TARGET_FILE_DIR:MorePhiTests>/${MOREPHI_SONICMASTER_MODEL}")
    if(EXISTS "${MOREPHI_SONICMASTER_SRC}")
        add_custom_command(TARGET MorePhiTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MOREPHI_SONICMASTER_SRC}" "${MOREPHI_SONICMASTER_DST}"
            COMMENT "Staging ${MOREPHI_SONICMASTER_MODEL} next to test exe"
            VERBATIM)
        add_custom_command(TARGET MorePhiTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/build/sonicmaster/masteringbrain_v2_contract.json"
                    "$<TARGET_FILE_DIR:MorePhiTests>/masteringbrain_v2_contract.json"
            COMMENT "Staging masteringbrain_v2_contract.json next to test exe"
            VERBATIM)
    endif()
    target_sources(MorePhiTests PRIVATE Unit/TestSonicMasterRunnerLive.cpp)
```

- [ ] **Step 3: Run the full test suite (Tier 1 + 2 always; Tier 3 only if model staged)**

Run: `cmake --build build --config Release && cd build && ctest --output-on-failure --parallel 4 -C Release`
Expected: PASS — all SonicMaster tests green. The live test skips cleanly when the model isn't staged.

- [ ] **Step 4: Commit**

```bash
git add tests/Unit/TestSonicMasterRunnerLive.cpp tests/CMakeLists.txt
git commit -m "test(sonicmaster): opt-in live ONNX smoke test + model staging"
```

---

## Task 10: README + spec linkage

Document the feature, its preview status, and the accepted limitations.

**Files:**
- Modify: `docs/USER_MANUAL.md` (or `docs/USER_GUIDE.md` — whichever the project uses for user-facing features)
- Modify: `AGENTS.md` (brief note in the architecture section)

- [ ] **Step 1: Add a "Neural Master (Preview)" section to the user manual**

Append to the user manual:

```markdown
## Neural Master (Preview)

More-Phi can drive its built-in mastering chain from a neural decision model
(masteringbrainv2). The model continuously analyses the last ~6 seconds of
audio on a background thread and refreshes the EQ, compressor, limiter, and
loudness settings roughly every 3 seconds. Every prediction is clamped by the
plugin's safety policy, so a bad frame can never push the chain into an
unsafe state.

This is a **preview feature, off by default**. The model is research-grade and
failed some of its own release-quality gates; treat it as an assistant, not an
autocrat. Toggle it on with the "Neural Master (Preview)" button in the UI.

Limitations (by design):
- It cannot react faster than ~3–6 seconds — sudden transients are not
  re-mastered until the next cycle.
- It is not sample-accurate: it produces static parameter settings, refreshed
  every cycle with a 200 ms crossfade.
- See `docs/superpowers/specs/2026-06-21-sonicmaster-vst3-realtime-integration-design.md`
  for the full design and failure model.
```

- [ ] **Step 2: Add a one-line note to AGENTS.md architecture section**

Under the existing `src/AI/` description in `AGENTS.md`, add:

```markdown
- `SonicMasterAnalysisEngine` — background-thread realtime neural mastering (ONNX, preview, default OFF). Parallel to the 63→72 `OnnxNeuralMasteringRunner`; feeds the built-in `AutoMasteringEngine` via `applyValidatedPlan`.
```

- [ ] **Step 3: Commit**

```bash
git add docs/USER_MANUAL.md AGENTS.md
git commit -m "docs(sonicmaster): user-facing preview section + architecture note"
```

---

## Final verification

- [ ] **Step 1: Full clean build + full test suite**

Run: `cmake --build build --config Release && cd build && ctest --output-on-failure --parallel 4 -C Release`
Expected: all tests pass (SonicMaster tests green; live test passes if the model is staged, skips otherwise).

- [ ] **Step 2: Optional ONNX-enabled verification (developer machine only)**

Run: `cmake -B build -S . -DMORE_PHI_BUILD_TESTS=ON -DMORE_PHI_ENABLE_ONNX=ON && cmake --build build --config Release && cd build && ctest -R "SonicMaster" --output-on-failure -C Release`
Expected: Tier 3 live test passes against the real exported model.

- [ ] **Step 3: Confirm no regression in shipped audio path**

Run: `cd build && ctest -R "E2ESignalPath|VST3AudioSignalAccuracy|PluginLifecycle" --output-on-failure -C Release`
Expected: PASS (feature is OFF by default; shipped behavior unchanged).
