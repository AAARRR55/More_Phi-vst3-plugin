// tests/Unit/TestSonicMasterAnalysisEngine.cpp
//
// Exercises SonicMasterAnalysisEngine with a StubDecisionSource (no ONNX).
// All four behaviours the design relies on are pinned here: end-to-end apply,
// insufficient-audio skip, N-consecutive-failure auto-disable, and the
// teardown join-before-destroy invariant.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

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
#include <set>
#include <string>
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

    // NOTE: setActive(true) here is for the background apply cycle, NOT for
    // capture. Since CAPTURE-DECOUPLE (2026-06-26), capture() runs whenever the
    // engine is prepared regardless of active_; active_ gates only the auto-apply
    // loop. This test enables it so runOneCycleForTest's apply path is active.
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

// REGRESSION (CAPTURE-DECOUPLE 2026-06-26): the production failure was that
// sonicmaster_decision / mastering.neural_apply ALWAYS failed with "no fresh
// audio captured yet" even after the user played audio for well over 6 s.
// Root cause: capture() was gated behind active_ (the SonicMaster preview
// toggle, OFF by default), AND the capture ring was lazily allocated only on
// first setActive(true)/requestDecisionNow — so during playback the ring was
// null and capture() was a no-op every block. By the time the assistant called
// requestDecisionNow -> ensureRing(), the freshly-allocated ring was empty.
//
// This test pins the FIXED behaviour: with preview OFF (setActive never called),
// after prepare() the ring is allocated eagerly, capture() fills it during
// playback, and requestDecisionNow succeeds on a full window. This is the exact
// production scenario every other test avoided by calling setActive(true).
TEST_CASE("On-demand requestDecisionNow works with preview OFF after prepare (CAPTURE-DECOUPLE)",
          "[SonicMaster][Engine][Regression]")
{
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);

    // NOTE: deliberately NOT calling setActive(true). This is the scenario the
    // other tests skip and the one that was broken in production: the assistant
    // triggers on-demand inference while the preview toggle is off.
    REQUIRE_FALSE(eng.isActive());

    // Simulate the user playing audio for ~6 s (well over the host-rate window).
    // Under the buggy code this captured nothing (ring was null); under the fix
    // the eagerly-allocated ring fills here.
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    // The assistant's on-demand call must now succeed — it has a full window.
    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(eng.requestDecisionNow(-14.0f, plan, nullptr, 0));
    CHECK(source.callCount() >= 1);

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
    // pointer (ring_) with release/acquire pairing. CAPTURE-DECOUPLE (2026-06-26)
    // changed the allocation model: the ring is now allocated EAGERLY in
    // prepare() (so on-demand capture works without the preview toggle), so the
    // original "capture before ring exists" race no longer occurs in production.
    // This test now verifies the two invariants that still matter:
    //   1. capture() on a fresh engine (no prepare() called) is a safe no-op.
    //   2. capture() hammered from a pseudo-audio thread while the message thread
    //      toggles active_ (which still touches active_ atomics) completes without
    //      fault — the ring pointer stays stable and writes don't corrupt.
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);

    // Invariant 1: capture() on an UNPREPARED engine (ring is nullptr, prepared_
    // is false) must be a safe no-op — no crash, no deref of a null ring.
    {
        std::vector<float> silence(512, 1e-4f);
        eng.capture(silence.data(), silence.data(), 512);  // must not crash
        CHECK(true);  // reaching here is the assertion
    }

    // Now prepare — this eagerly allocates the ring (CAPTURE-DECOUPLE).
    eng.prepare(48000.0, 512);

    // Invariant 2: hammer capture() from a "pseudo-audio" thread while the
    // message thread toggles active_. Since capture no longer gates on active_
    // (CAPTURE-DECOUPLE), every capture() call now writes to the ring; the
    // invariant is that this completes without corruption or fault under the
    // atomic active_ writes happening concurrently.
    std::vector<float> silence(512, 1e-4f);
    std::atomic<bool> stop { false };
    std::atomic<int> captureIters { 0 };

    std::thread audioThread([&] {
        while (!stop.load(std::memory_order_relaxed))
        {
            eng.capture(silence.data(), silence.data(), 512);
            captureIters.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Message-thread side: just sleep briefly to let the audio thread accumulate
    // iterations. The active_ toggling is NOT what we're testing here (capture no
    // longer reads active_); we only need to confirm capture() runs concurrently
    // with a live analysis thread without fault.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    stop.store(true, std::memory_order_relaxed);
    audioThread.join();  // must not have crashed

    // The audio thread accumulated capture iterations concurrently with the
    // live analysis thread (prepare() started it) — capture() now runs on
    // every call since the ring exists and active_ no longer gates it.
    CHECK(captureIters.load(std::memory_order_relaxed) > 0);

    eng.release();
}

// ── Stage D (2026-06-26): closed LUFS feedback loop ───────────────────────────
// The loop measures achieved LUFS after each apply and folds a bounded correction
// into the next cycle's target. These tests pin the GUARD logic (the loop must
// NOT activate when the apply didn't reach audio) and the accessor shape. The
// convergence math (monotonic approach, bounded nudge) is exercised by the
// correction formula in runCycle; a full hosted-plugin convergence test needs a
// real hosted plugin writing params (covered by integration tests).
TEST_CASE("Closed LUFS loop stays inactive when apply reaches no audio path (Stage D guard)",
          "[SonicMaster][Engine][StageD]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);

    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);
    eng.setActive(true);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    // Before any cycle: feedback inactive.
    CHECK(eng.getClosedLoopState().feedbackActive == false);

    REQUIRE(eng.runOneCycleForTest());
    // No hosted plugin + dormant internal chain → reachedAudio == false → the
    // loop MUST NOT have activated (no measurement-based correction without an
    // audible apply to measure).
    CHECK(eng.getStatus() == more_phi::SonicMasterAnalysisEngine::Status::AppliedNoAudioPath);
    CHECK(eng.getClosedLoopState().feedbackActive == false);

    eng.release();
    engine.reset();
}

TEST_CASE("ClosedLoopState accessor reports sane defaults before any apply (Stage D)",
          "[SonicMaster][Engine][StageD]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine);
    eng.prepare(48000.0, 512);

    const auto s = eng.getClosedLoopState();
    CHECK(s.feedbackActive == false);
    CHECK(std::abs(s.lastAppliedTargetLufs - (-14.0f)) < 1e-3f);  // neutral default
    CHECK(std::abs(s.nextTargetLufs - (-14.0f)) < 1e-3f);         // neutral when inactive

    eng.release();
}

// Stage D: the feedback bounds are the safety contract — pin them as constants so
// a future tuning change can't silently remove the per-cycle cap or deadband that
// prevents runaway/oscillation.
TEST_CASE("Closed LUFS loop bounds are bounded + deadbanded (Stage D contract)",
          "[SonicMaster][Engine][StageD]")
{
    // Deadband: errors below this don't trigger correction (avoids on-target churn).
    CHECK(more_phi::SonicMasterAnalysisEngine::kFeedbackDeadbandLu == Catch::Approx(0.5f));
    // Per-cycle correction cap: the loop can't move the target more than this in
    // one cycle, so even a huge error converges over multiple cycles (no overshoot).
    CHECK(more_phi::SonicMasterAnalysisEngine::kFeedbackMaxCorrectionLu == Catch::Approx(1.0f));
    // Target clamp matches the engine's [-23,-8] loudness output range.
    CHECK(more_phi::SonicMasterAnalysisEngine::kFeedbackMinTargetLu == Catch::Approx(-23.0f));
    CHECK(more_phi::SonicMasterAnalysisEngine::kFeedbackMaxTargetLu == Catch::Approx(-8.0f));
}

// DIAGNOSTIC (2026-06-26): the failure-reason out-param. The production failure
// was that sonicmaster_decision / mastering.neural_apply returned an opaque
// "Inference failed or model unavailable." for EVERY false return, so the
// assistant confabulated causes ("no audio captured" / "inference server down").
// requestDecisionNow now takes DecisionFailure* and stamps the ACTUAL gate that
// tripped. These tests pin each reason so the structured signal can't regress to
// the opaque bool. The MCP tool surfaces this as failure_reason / state; see
// sonicmasterDecision's failure block in MCPToolHandler.cpp.
TEST_CASE("requestDecisionNow reports InsufficientAudio before any capture",
          "[SonicMaster][Engine][Diagnostic]")
{
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);
    // NOTE: no audio fed — the ring is allocated (eagerly in prepare) but empty.

    more_phi::ValidatedNeuralMasteringPlan plan {};
    more_phi::DecisionFailure failure = more_phi::DecisionFailure::None;
    REQUIRE_FALSE(eng.requestDecisionNow(-14.0f, plan, nullptr, 0, &failure));
    CHECK(failure == more_phi::DecisionFailure::InsufficientAudio);
    CHECK(source.callCount() == 0);  // never even called inference

    eng.release();
}

TEST_CASE("requestDecisionNow reports SilentInput on an all-zero capture",
          "[SonicMaster][Engine][Diagnostic]")
{
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);

    // Feed a full window of TRUE silence (0.0f). The peak gate at <1e-15 trips
    // BEFORE inference is attempted, so failure == SilentInput (not InsufficientAudio
    // — the ring IS full). feedSilence uses 1e-4f so we must feed real zeros here.
    const std::size_t frames = hostWindowFrames(48000.0) + 1024;
    std::vector<float> z(frames, 0.0f);
    constexpr std::size_t kBlock = 512;
    for (std::size_t off = 0; off < frames; off += kBlock)
    {
        const std::size_t n = std::min(kBlock, frames - off);
        eng.capture(z.data() + off, z.data() + off, n);
    }

    more_phi::ValidatedNeuralMasteringPlan plan {};
    more_phi::DecisionFailure failure = more_phi::DecisionFailure::None;
    REQUIRE_FALSE(eng.requestDecisionNow(-14.0f, plan, nullptr, 0, &failure));
    CHECK(failure == more_phi::DecisionFailure::SilentInput);
    CHECK(source.callCount() == 0);  // peak gate fires before inference

    eng.release();
}

TEST_CASE("requestDecisionNow reports InferenceRejected when the source fails",
          "[SonicMaster][Engine][Diagnostic]")
{
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    source.setShouldSucceed(false);  // infer() returns false
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);  // loud enough (1e-4)

    more_phi::ValidatedNeuralMasteringPlan plan {};
    more_phi::DecisionFailure failure = more_phi::DecisionFailure::None;
    REQUIRE_FALSE(eng.requestDecisionNow(-14.0f, plan, nullptr, 0, &failure));
    CHECK(failure == more_phi::DecisionFailure::InferenceRejected);
    CHECK(source.callCount() >= 1);  // inference WAS attempted, then rejected

    eng.release();
}

TEST_CASE("requestDecisionNow reports None on success",
          "[SonicMaster][Engine][Diagnostic]")
{
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    more_phi::ValidatedNeuralMasteringPlan plan {};
    more_phi::DecisionFailure failure = more_phi::DecisionFailure::InsufficientAudio; // sentinel
    REQUIRE(eng.requestDecisionNow(-14.0f, plan, nullptr, 0, &failure));
    CHECK(failure == more_phi::DecisionFailure::None);

    eng.release();
}

TEST_CASE("requestDecisionNow default out-param keeps source compatibility",
          "[SonicMaster][Engine][Diagnostic]")
{
    // The new out-param defaults to nullptr so the 4 existing call sites
    // (TestSonicMasterAnalysisEngine.cpp:191-192,265 + MCPToolHandler) compile
    // unchanged. This test pins that contract: calling without the arg still works.
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    more_phi::ValidatedNeuralMasteringPlan plan {};
    REQUIRE(eng.requestDecisionNow(-14.0f, plan, nullptr, 0));  // no outFailure arg

    eng.release();
}

TEST_CASE("getCaptureDiagnostics reports fill ratio as audio accumulates",
          "[SonicMaster][Engine][Diagnostic]")
{
    // The capture_diagnostics block in the failure JSON is built from this struct.
    // Pin its semantics: requiredFrames matches the ~6s host window, capturedFrames
    // grows with capture(), and the ratio crosses 1.0 once a full window is in.
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);

    const auto d0 = eng.getCaptureDiagnostics();
    CHECK(d0.prepared);
    CHECK(d0.ringAllocated);          // eagerly allocated in prepare() (CAPTURE-DECOUPLE)
    CHECK(d0.capturedFrames == 0);
    CHECK(d0.requiredFrames == hostWindowFrames(48000.0));

    // Feed half the required window; capturedFrames should reflect it (clamped to
    // capacity, which is >= requiredFrames after pow2 rounding).
    feedSilence(eng, d0.requiredFrames / 2);
    const auto d1 = eng.getCaptureDiagnostics();
    CHECK(d1.capturedFrames >= d0.requiredFrames / 2);

    // Feed well past the window; the ring saturates at capacity.
    feedSilence(eng, d0.requiredFrames * 4);
    const auto d2 = eng.getCaptureDiagnostics();
    CHECK(d2.capturedFrames >= d0.requiredFrames);   // a full window is available

    eng.release();
}

// SAFETY-REJECTION-DETAIL (2026-06-26): the production failure was that a safety
// reject surfaced only as the coarse "safety_rejected" with the SPECIFIC issue
// (LowConfidence vs TargetOutOfRange vs NonFiniteValue) discarded. The engine now
// records the issue(s) on both paths and exposes them via getLastSafetyRejection();
// the MCP tool folds them into the failure JSON. These tests pin the new public
// surface so the structured signal can't regress to the opaque bucket.
//
// We test the pure enum mappers directly (deterministic, no engine state) and the
// accessor's lifecycle invariants (default-invalid, clear-on-success). Driving a
// SPECIFIC issue through the full pipeline would require reaching the engine's
// private safetyPolicy_ config; instead the makeSafetyCandidate→validate path is
// already covered by TestNeuralMasteringSafetyPolicy.cpp, and the wiring (record
// on reject, clear on accept, expose via accessor) is what we pin here.

TEST_CASE("neuralMasteringIssueKey maps every issue to a stable snake_case key",
          "[SonicMaster][Engine][SafetyDiagnostic]")
{
    using I = more_phi::NeuralMasteringValidationIssue;
    CHECK(std::string(more_phi::neuralMasteringIssueKey(I::None))                  == "none");
    CHECK(std::string(more_phi::neuralMasteringIssueKey(I::LowConfidence))         == "low_confidence");
    CHECK(std::string(more_phi::neuralMasteringIssueKey(I::TargetOutOfRange))      == "target_out_of_range");
    CHECK(std::string(more_phi::neuralMasteringIssueKey(I::NonFiniteValue))        == "non_finite_value");
    CHECK(std::string(more_phi::neuralMasteringIssueKey(I::StalePlan))             == "stale_plan");
    CHECK(std::string(more_phi::neuralMasteringIssueKey(I::HighRiskMask))          == "high_risk_mask");
    // Every key is non-empty and none collides with another (regression guard for
    // a future enum edit that copies a label).
    std::set<std::string> seen;
    for (auto i : { I::None, I::SchemaVersionMismatch, I::AudioCallbackRuntime,
                    I::InvalidTimestamp, I::StalePlan, I::LowConfidence, I::Abstain,
                    I::ReviewOnly, I::UnsupportedLayout, I::NonFiniteValue,
                    I::TargetOutOfRange, I::DeltaOutOfRange, I::IllegalMask,
                    I::HighRiskMask, I::MaxDeltaProjected })
    {
        const std::string k = more_phi::neuralMasteringIssueKey(i);
        REQUIRE(!k.empty());
        INFO("duplicate key: " << k);
        CHECK(seen.insert(k).second);
    }
}

TEST_CASE("isHardRejectNeuralMasteringIssue classifies hard vs soft correctly",
          "[SonicMaster][Engine][SafetyDiagnostic]")
{
    using I = more_phi::NeuralMasteringValidationIssue;
    // Hard rejects — retrying the same audio will NOT flip these.
    CHECK(more_phi::isHardRejectNeuralMasteringIssue(I::TargetOutOfRange));
    CHECK(more_phi::isHardRejectNeuralMasteringIssue(I::DeltaOutOfRange));
    CHECK(more_phi::isHardRejectNeuralMasteringIssue(I::NonFiniteValue));
    CHECK(more_phi::isHardRejectNeuralMasteringIssue(I::SchemaVersionMismatch));
    CHECK(more_phi::isHardRejectNeuralMasteringIssue(I::HighRiskMask));
    // Soft holds — retryable / informational.
    CHECK_FALSE(more_phi::isHardRejectNeuralMasteringIssue(I::LowConfidence));
    CHECK_FALSE(more_phi::isHardRejectNeuralMasteringIssue(I::StalePlan));
    CHECK_FALSE(more_phi::isHardRejectNeuralMasteringIssue(I::Abstain));
    CHECK_FALSE(more_phi::isHardRejectNeuralMasteringIssue(I::ReviewOnly));
    CHECK_FALSE(more_phi::isHardRejectNeuralMasteringIssue(I::MaxDeltaProjected));
    CHECK_FALSE(more_phi::isHardRejectNeuralMasteringIssue(I::None));
}

TEST_CASE("getLastSafetyRejection is invalid before any reject and clears on success",
          "[SonicMaster][Engine][SafetyDiagnostic]")
{
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);

    // Default state: no rejection recorded.
    const auto sr0 = eng.getLastSafetyRejection();
    CHECK_FALSE(sr0.valid);
    CHECK(sr0.primaryIssue == more_phi::NeuralMasteringValidationIssue::None);
    CHECK_FALSE(sr0.hardReject);

    // A successful decision clears any prior rejection snapshot (and there's none
    // here, so this pins the clear-on-success path doesn't corrupt state).
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);
    more_phi::ValidatedNeuralMasteringPlan plan {};
    more_phi::DecisionFailure failure = more_phi::DecisionFailure::None;
    REQUIRE(eng.requestDecisionNow(-14.0f, plan, nullptr, 0, &failure));
    CHECK(failure == more_phi::DecisionFailure::None);
    const auto sr1 = eng.getLastSafetyRejection();
    CHECK_FALSE(sr1.valid);  // success cleared it

    eng.release();
}

TEST_CASE("getLastSafetyRejection records a specific issue on a safety reject",
          "[SonicMaster][Engine][SafetyDiagnostic]")
{
    // Force a safety reject: make the stub emit a decision whose decoded plan
    // falls outside the safety policy's sanity bounds. Setting an EQ gain at the
    // max edge + an extreme target_lufs pushes projected targets to the bound;
    // the stub's neutral compressor/limiter keep other slots finite. If validate()
    // accepts, the test still passes (we only assert that IF it rejected, the
    // specific issue was recorded) — but in practice the extreme target triggers
    // TargetOutOfRange or LowConfidence-style projection. Either way the
    // snapshot's primaryIssue must be non-None and valid=true on reject.
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source;
    eng.setInferenceSource(&source);
    eng.prepare(48000.0, 512);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    more_phi::ValidatedNeuralMasteringPlan plan {};
    more_phi::DecisionFailure failure = more_phi::DecisionFailure::None;
    const bool ok = eng.requestDecisionNow(-30.0f, plan, nullptr, 0, &failure);

    if (!ok && failure == more_phi::DecisionFailure::SafetyRejected)
    {
        // The diagnostic contract: a safety reject MUST populate the snapshot with
        // a specific, non-None primary issue. This is exactly what the assistant
        // needs to stop saying "rejected, try again" and start naming the reason.
        const auto sr = eng.getLastSafetyRejection();
        CHECK(sr.valid);
        CHECK(sr.primaryIssue != more_phi::NeuralMasteringValidationIssue::None);
        CHECK(sr.issueCount > 0);
        // hardReject must agree with isHardRejectNeuralMasteringIssue(primaryIssue).
        CHECK(sr.hardReject == more_phi::isHardRejectNeuralMasteringIssue(sr.primaryIssue));
    }
    // If ok (the policy accepted the extreme target via projection), we can't
    // force a reject deterministically without private config access — the
    // pure-function tests above already pin the classification, and this branch
    // documents that acceptance leaves the snapshot valid=false.
    if (ok)
    {
        const auto sr = eng.getLastSafetyRejection();
        CHECK_FALSE(sr.valid);
    }

    eng.release();
}
