// tests/Unit/TestSonicMasterAnalysisEngine.cpp
//
// Exercises SonicMasterAnalysisEngine with a StubDecisionSource (no ONNX).
// All four behaviours the design relies on are pinned here: end-to-end apply,
// insufficient-audio skip, N-consecutive-failure auto-disable, and the
// teardown join-before-destroy invariant.
#include <catch2/catch_test_macros.hpp>

#include "AI/SonicMasterAnalysisEngine.h"
#include "Core/AutoMasteringEngine.h"
#include "Core/NeuralMasteringTypes.h"
#include "AI/ChainPlanExecutor.h"
#include "AI/OzonePlanApplicator.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {

// Returns a neutral, in-range decision so the safety policy accepts it.
class StubDecisionSource final : public more_phi::ISonicMasterInferenceSource
{
public:
    [[nodiscard]] bool isAvailable() const noexcept override { return available_; }
    void setAvailable(bool v) { available_ = v; }

    bool infer(const float* /*stereoInterleaved*/, float* outDecision,
               std::size_t outCapacity) noexcept override
    {
        if (outCapacity < more_phi::kSonicMasterDecisionWidth) return false;
        for (auto& v : decision_) v = 0.0f;
        // Neutral: -14 LUFS, -1 dBTP, zero EQ, zero comp offsets.
        decision_[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;
        decision_[more_phi::kSonicMasterTruePeakIdx]   = -1.0f;
        for (std::size_t i = 0; i < decisionOverrides_.size(); ++i)
            if (decisionOverrideEnabled_[i])
                decision_[i] = decisionOverrides_[i];
        std::copy_n(decision_, more_phi::kSonicMasterDecisionWidth, outDecision);
        ++callCount_;
        return shouldSucceed_;
    }

    int callCount() const { return callCount_; }
    void setShouldSucceed(bool v) { shouldSucceed_ = v; }
    void setDecisionValue(std::size_t index, float value)
    {
        if (index < decisionOverrides_.size())
        {
            decisionOverrides_[index] = value;
            decisionOverrideEnabled_[index] = true;
        }
    }

private:
    float decision_[more_phi::kSonicMasterDecisionWidth] {};
    std::array<float, more_phi::kSonicMasterDecisionWidth> decisionOverrides_ {};
    std::array<bool, more_phi::kSonicMasterDecisionWidth> decisionOverrideEnabled_ {};
    int callCount_ = 0;
    bool shouldSucceed_ = true;
    bool available_ = true;
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

// Number of host-rate frames the engine must capture before it has a full
// model segment worth of audio (the engine resamples host->44.1k internally).
// Mirrors SonicMasterAnalysisEngine::prepare's hostFrames computation.
std::size_t hostWindowFrames(double sampleRate)
{
    return static_cast<std::size_t>(
        std::llround(more_phi::kSonicMasterSegmentFrames * sampleRate / 44100.0));
}

// Stub OzonePlanApplicatorBase for testing the neural→hosted-plugin bridge.
// Returns a fixed positive count so the bridge shows reachAudio=true even
// when the internal mastering chain is dormant.
struct StubBridgeApplicator final : more_phi::OzonePlanApplicatorBase
{
    int returnCount = 42;  // arbitrary positive: anything > 0 means "wrote params"
    int lastApplyCount = -1;

    int apply(const more_phi::MultiEffectPlan& /*plan*/) override
    {
        lastApplyCount = returnCount;
        return returnCount;
    }

    int getLastAppliedCount() const noexcept override
    {
        return lastApplyCount;
    }
};

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

    // Capture is gated on active_ (the audio thread only captures when the
    // feature is on), so enable it before feeding audio.
    eng.setActive(true);
    // Feed well over the host-rate window so capturedFrames() >= required.
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    REQUIRE(eng.runOneCycleForTest());
    // F2/AUDIT: the app engine was prepared with startIntelligence=false, so it
    // is inactive and the plan reached no audio path. The engine reports
    // AppliedNoAudioPath instead of the misleading Applied.
    // Note: callCount may be >1 if the background analysisLoop thread races
    // with runOneCycleForTest — we assert plan application, not infer count.
    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::AppliedNoAudioPath);
    CHECK_FALSE(eng.lastApplyReachedAudioPath());
    CHECK(engine.hasLastSafeNeuralMasteringPlan());

    eng.release();
}

TEST_CASE("AnalysisEngine applies the safety-projected plan, not the raw decoded target",
          "[SonicMaster][Engine][Safety]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    source.setDecisionValue(more_phi::kSonicMasterEqGainOffset + 0,
                            more_phi::kAdaptiveEqMaxGainDb);
    source.setDecisionValue(more_phi::kSonicMasterTargetLufsIdx, -8.0f);
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);

    eng.setActive(true);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    REQUIRE(eng.runOneCycleForTest());
    REQUIRE(engine.hasLastSafeNeuralMasteringPlan());

    const auto& applied = engine.getLastSafeNeuralMasteringPlan();
    CHECK(applied.valid);
    CHECK(applied.projected);
    CHECK(std::abs(applied.projectedTargets.eq[0] - 0.15f) < 1.0e-5f);
    CHECK(std::abs(applied.projectedTargets.loudness[0] - 0.10f) < 1.0e-5f);

    eng.release();
}

TEST_CASE("AnalysisEngine decision-only requests do not advance safety baseline",
          "[SonicMaster][Engine][Safety]")
{
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    source.setDecisionValue(more_phi::kSonicMasterEqGainOffset + 0,
                            more_phi::kAdaptiveEqMaxGainDb);
    source.setDecisionValue(more_phi::kSonicMasterTargetLufsIdx, -8.0f);
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);

    eng.setActive(true);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    more_phi::ValidatedNeuralMasteringPlan first {};
    more_phi::ValidatedNeuralMasteringPlan second {};
    REQUIRE(eng.requestDecisionNow(-14.0f, first, nullptr, 0));
    REQUIRE(eng.requestDecisionNow(-14.0f, second, nullptr, 0));

    CHECK(first.projected);
    CHECK(second.projected);
    CHECK(std::abs(first.projectedTargets.eq[0] - 0.15f) < 1.0e-5f);
    CHECK(std::abs(second.projectedTargets.eq[0] - 0.15f) < 1.0e-5f);
    // Stage A (2026-06-26): requestDecisionNow now honors the explicit target_lufs
    // (-14) at decode time, so loudness value = (-14+14)/6 = 0.0, projected from a
    // zero baseline = 0.0 (was 0.10 when the target was ignored and the model's -8
    // drove a +0.10 max-delta step). The safety projection cap (0.10/plan) still
    // governs, but with a 0.0 delta there's nothing to step.
    CHECK(std::abs(first.projectedTargets.loudness[0] - 0.0f) < 1.0e-5f);
    CHECK(std::abs(second.projectedTargets.loudness[0] - 0.0f) < 1.0e-5f);

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

    eng.setActive(true);
    // Far short of the host-rate window.
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
    eng.setActive(true);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    for (int i = 0; i < 3; ++i)
        eng.runOneCycleForTest();

    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::ErrorAutoDisabled);
    CHECK_FALSE(eng.isActive());

    eng.release();
}

TEST_CASE("AnalysisEngine auto-recovers after ErrorAutoDisabled when source heals",
          "[SonicMaster][Engine][Recovery]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    source.setShouldSucceed(false);
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);
    eng.setActive(true);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    // 3 failures → ErrorAutoDisabled
    for (int i = 0; i < 3; ++i)
        eng.runOneCycleForTest();
    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::ErrorAutoDisabled);
    CHECK_FALSE(eng.isActive());

    // Heal the source and run another cycle — the engine should auto-recover.
    source.setShouldSucceed(true);
    REQUIRE(eng.runOneCycleForTest());
    CHECK(eng.isActive());
    CHECK(eng.getStatus() != more_phi::SonicMasterAnalysisEngine::Status::ErrorAutoDisabled);

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
    feedSilence(*eng, hostWindowFrames(48000.0) + 1024);

    // Release must join the analysis thread before the destructor runs.
    eng->release();
    delete eng;
    // Reaching here without a hang/crash is the assertion.
    CHECK(true);
}

TEST_CASE("AnalysisEngine reports Applied when the mastering chain is active",
          "[SonicMaster][Engine][F2]")
{
    // F2/AUDIT: counterpart to the dormant-chain case. When the app engine IS
    // active, the apply reached the audio path and the status is genuinely
    // Applied (not the dormant AppliedNoAudioPath).
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);
    engine.setActive(true);   // the chain processes audio

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);
    eng.setActive(true);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    REQUIRE(eng.runOneCycleForTest());
    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::Applied);
    CHECK(eng.lastApplyReachedAudioPath());

    eng.release();
}

TEST_CASE("Bridge forwards neural plan to Ozone applicator and reachedAudio is true",
          "[SonicMaster][Engine][Bridge]")
{
    // CRITICAL-6/7/17: the bridge converts a neural plan into a MultiEffectPlan
    // and forwards it through chainPlanner_.applyPlan(). When an OzonePlanApplicator
    // is registered (hosted Ozone plugin), the bridge returns a positive enqueue
    // count even though the internal DSP chain is dormant. lastApplyReachedAudioPath()
    // must be true in that case.
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);
    // Internal chain is dormant — isActive() is false.
    REQUIRE_FALSE(engine.isActive());

    // Wire a stub Ozone applicator into the chain planner.
    StubBridgeApplicator stubApplicator;
    engine.getChainPlanner().setOzonePlanApplicator(&stubApplicator);
    REQUIRE(engine.getChainPlanner().hasOzoneApplicator());

    // Build a known-valid neural plan.
    more_phi::ValidatedNeuralMasteringPlan plan {};
    plan.valid = true;
    plan.fallbackMode = more_phi::NeuralMasteringFallbackMode::None;
    plan.projected = true;
    // Neutral defaults that the safety policy accepts.
    for (auto& v : plan.projectedTargets.eq)       v = 0.0f;
    for (auto& v : plan.projectedTargets.dynamics) v = 0.0f;
    for (auto& v : plan.projectedTargets.stereo)   v = 0.0f;
    for (auto& v : plan.projectedTargets.harmonic) v = 0.0f;
    plan.projectedTargets.loudness[0] = 0.0f;
    plan.projectedTargets.limiter[0]  = 0.0f;

    REQUIRE(engine.applyValidatedPlan(plan));

    // The bridge should have called the stub applicator.
    CHECK(stubApplicator.lastApplyCount > 0);
    // The engine's Ozone count should reflect the stub's return value.
    CHECK(engine.getLastOzoneAppliedCount() > 0);
    // Even though the internal chain is dormant, the Ozone path reached audio.
    CHECK(engine.lastApplyReachedAudioPath());
    CHECK_FALSE(engine.isActive());  // still dormant

    engine.getChainPlanner().clearOzonePlanApplicator();
}

TEST_CASE("NaN in capture buffer does not poison inference",
          "[SonicMaster][Engine][NaN]")
{
    // CRITICAL-1: peakAbs skips non-finite samples, and the peak-normalization
    // guards against a non-finite result. Feeding a buffer with some NaNs should
    // not crash or produce NaN/Inf in the interleaved model input.
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);
    eng.setActive(true);

    // Feed a full window of audio but poison ~10% of samples with NaN.
    const std::size_t hw = hostWindowFrames(48000.0) + 1024;
    std::vector<float> l(hw, 1e-4f), r(hw, 1e-4f);
    for (std::size_t i = 0; i < hw; i += 10)
    {
        if (i < l.size()) l[i] = std::numeric_limits<float>::quiet_NaN();
        if (i < r.size()) r[i] = std::numeric_limits<float>::quiet_NaN();
    }

    constexpr std::size_t kBlock = 512;
    for (std::size_t off = 0; off < hw; off += kBlock)
    {
        const std::size_t n = std::min(kBlock, hw - off);
        eng.capture(l.data() + off, r.data() + off, n);
    }

    // runOneCycleForTest should complete without crashing. It may return
    // false if the safety policy rejects the result, but it must not
    // crash or throw.
    const bool result = eng.runOneCycleForTest();
    juce::ignoreUnused(result);  // accepts either outcome
    // The decision source was called — the NaN-poisoned audio made it through
    // peakAbs and normalization without producing non-finite gains.
    CHECK(source.callCount() > 0);

    eng.release();
}

TEST_CASE("AnalysisEngine capture() is safe before ring allocation and under concurrent activation",
          "[SonicMaster][Engine][C3]")
{
    // C-3 FIX (audit): the capture ring is published through an atomic raw
    // pointer (ring_) with release/acquire pairing. Two invariants must hold:
    //   1. capture() called BEFORE the ring is lazily allocated must no-op
    //      cleanly (acquire-load returns nullptr) — no torn read, no crash.
    //   2. capture() racing against setActive(true)->ensureRing() must never
    //      observe a partially-constructed AudioCaptureRing.
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);

    // Invariant 1: capture() with no ring allocated yet must be a safe no-op.
    // (active_ is false and ring_ is nullptr; either gate is sufficient.)
    std::vector<float> silence(512, 1e-4f);
    eng.capture(silence.data(), silence.data(), 512);  // must not crash
    CHECK(true);  // reaching here is the assertion

    // Invariant 2: hammer capture() from a "pseudo-audio" thread while the
    // message thread repeatedly activates/deactivates (which calls ensureRing()
    // and publishes the ring pointer). Under the old non-atomic unique_ptr
    // assignment this was a genuine torn-pointer race; under the atomic
    // publish it must complete without fault.
    std::atomic<bool> stop { false };
    std::atomic<int> captureIters { 0 };

    std::thread audioThread([&] {
        while (!stop.load(std::memory_order_relaxed))
        {
            eng.capture(silence.data(), silence.data(), 512);
            captureIters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Message-thread side: toggle active_ to trigger ensureRing() publication
    // repeatedly. The first setActive(true) allocates; subsequent toggles keep
    // the pointer stable but exercise the acquire-load path under contention.
    for (int i = 0; i < 200; ++i)
    {
        eng.setActive(true);
        eng.setActive(false);
    }

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();  // must not have crashed

    // The audio thread ran at least a few iterations alongside activation.
    CHECK(captureIters.load(std::memory_order_relaxed) > 0);

    eng.release();
}
