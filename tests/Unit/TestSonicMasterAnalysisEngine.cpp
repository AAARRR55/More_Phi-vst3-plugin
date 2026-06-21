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

#include <algorithm>
#include <cmath>
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
        std::copy_n(decision_, more_phi::kSonicMasterDecisionWidth, outDecision);
        ++callCount_;
        return shouldSucceed_;
    }

    int callCount() const { return callCount_; }
    void setShouldSucceed(bool v) { shouldSucceed_ = v; }

private:
    float decision_[more_phi::kSonicMasterDecisionWidth] {};
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
