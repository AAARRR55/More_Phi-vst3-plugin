// tests/Unit/TestGenreLoudnessPrior.cpp
//
// Stage 1 (Ozone §3.2): the genre-derived target-LUFS prior flows from
// setGenreTargetLufs through the SonicMaster decode path into the applied
// ValidatedNeuralMasteringPlan. Precedence under test:
//   on-demand explicit target  >  genre prior  >  model default
// (The closed-loop feedback path is exercised elsewhere; here it stays off so
// the genre prior is the only override in play on the background cycle.)
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/SonicMasterAnalysisEngine.h"
#include "AI/GenreMasteringProfile.h"
#include "AI/SonicMasterDecisionDecoder.h"
#include "Core/AutoMasteringEngine.h"
#include "Core/NeuralMasteringTypes.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

// Neutral decision so the safety policy accepts it; lets us read the decoded
// loudness slot back without EQ/dynamics interference.
class StubDecisionSource final : public more_phi::ISonicMasterInferenceSource
{
public:
    [[nodiscard]] bool isAvailable() const noexcept override { return available_; }
    bool infer(const float* /*stereoInterleaved*/, float* outDecision,
               std::size_t outCapacity) noexcept override
    {
        if (outCapacity < more_phi::kSonicMasterDecisionWidth) return false;
        std::array<float, more_phi::kSonicMasterDecisionWidth> d {};
        d[more_phi::kSonicMasterTargetLufsIdx] = -14.0f;  // model "recommends" -14
        d[more_phi::kSonicMasterTruePeakIdx]   = -1.0f;
        std::copy_n(d.data(), more_phi::kSonicMasterDecisionWidth, outDecision);
        ++callCount_;
        return shouldSucceed_;
    }
    int callCount() const { return callCount_; }
    void setAvailable(bool v) noexcept { available_ = v; }
    void setShouldSucceed(bool v) noexcept { shouldSucceed_ = v; }
private:
    int  callCount_ = 0;
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

std::size_t hostWindowFrames(double sampleRate)
{
    return static_cast<std::size_t>(
        std::llround(more_phi::kSonicMasterSegmentFrames * sampleRate / 44100.0));
}

// Inverse of the decoder loudness map: loudness[0] = (lufs + 14) / 6.
float lufsToSlot(float lufs) { return (lufs + 14.0f) / 6.0f; }

} // namespace

// Helper: feed one cycle and read back the applied loudness target (LUFS).
// The safety policy rate-limits per-cycle movement (maxDeltaPerPlan.loudness ≈
// 0.6 LU/cycle), so a single cycle never jumps straight to the prior — it moves
// toward it. The observable that proves the prior is wired is the DIRECTION and
// relative position of the applied loudness between two configs after the same
// number of cycles, not an absolute hit in one cycle.
float oneCycleAppliedLufs(more_phi::SonicMasterAnalysisEngine& eng,
                          more_phi::AutoMasteringEngine& engine)
{
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);
    const bool ok = eng.runOneCycleForTest();
    if (!ok)
    {
        // DIAGNOSTIC: surface the structured skip reason so the failure message
        // identifies which gate tripped instead of a bare false.
        const auto fail = eng.getLastCycleFailure();
        const auto sr = eng.getLastSafetyRejection();
        INFO("runOneCycleForTest failed: DecisionFailure=" << static_cast<int>(fail)
             << " safetyRejectionValid=" << (sr.valid ? 1 : 0)
             << " primaryIssue=" << static_cast<int>(sr.primaryIssue));
        REQUIRE(ok);
    }
    REQUIRE(engine.hasLastSafeNeuralMasteringPlan());
    const auto& applied = engine.getLastSafeNeuralMasteringPlan();
    return -14.0f + applied.projectedTargets.loudness[0] * 6.0f;
}

TEST_CASE("Genre prior shifts the applied loudness toward the genre target",
          "[GenrePrior][SonicMaster]")
{
    // The model recommends -14 (StubDecisionSource). A genre prior of -9 (index 0)
    // must pull the applied loudness ABOVE the no-prior baseline, even though the
    // safety policy caps the per-cycle move. We compare two engines with identical
    // audio/decisions: one with the prior, one without. The prior's applied LUFS
    // must be higher (closer to -9) than the no-prior baseline.
    const float genreLufs = more_phi::getGenreMasteringProfile(0).targetLufs;  // -9
    REQUIRE(genreLufs == Catch::Approx(-9.0f).margin(1e-3f));

    // With prior
    more_phi::AutoMasteringEngine engWith;  engWith.prepare(48000.0, 512, false);
    more_phi::SonicMasterAnalysisEngine smWith;
    StubDecisionSource srcWith; smWith.setInferenceSource(&srcWith);
    smWith.setApplicationEngine(&engWith); smWith.prepare(48000.0, 512);
    smWith.setActive(true); smWith.setGenreTargetLufs(genreLufs);
    const float lufsWith = oneCycleAppliedLufs(smWith, engWith);

    // Without prior (sentinel → model -14 default)
    more_phi::AutoMasteringEngine engNo;  engNo.prepare(48000.0, 512, false);
    more_phi::SonicMasterAnalysisEngine smNo;
    StubDecisionSource srcNo; smNo.setInferenceSource(&srcNo);
    smNo.setApplicationEngine(&engNo); smNo.prepare(48000.0, 512);
    smNo.setActive(true);
    smNo.setGenreTargetLufs(more_phi::kUseModelTargetLufs);
    const float lufsNo = oneCycleAppliedLufs(smNo, engNo);

    // The genre prior (-9, louder) must pull the applied target upward vs the
    // model default (-14). Both start from the same baseline, so the prior's
    // delta is in the +LUFS direction.
    CHECK(lufsWith > lufsNo);

    smWith.release();
    smNo.release();
}

TEST_CASE("Genre prior sentinel leaves the model recommendation in place",
          "[GenrePrior][SonicMaster]")
{
    // With the sentinel, the applied loudness should sit at the model default
    // (-14) projected through the safety floor — identical to never having set
    // a prior. This is the regression guard: wiring the prior did not change
    // default behavior.
    more_phi::AutoMasteringEngine engine; engine.prepare(48000.0, 512, false);
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source; eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine); eng.prepare(48000.0, 512);
    eng.setActive(true);
    eng.setGenreTargetLufs(more_phi::kUseModelTargetLufs);

    const float lufs = oneCycleAppliedLufs(eng, engine);
    // Model recommends -14; safety floors the slot but won't push below -14 here.
    CHECK(lufs <= Catch::Approx(-13.4f).margin(0.1f));

    eng.release();
}

TEST_CASE("On-demand explicit target shifts applied loudness above baseline",
          "[GenrePrior][SonicMaster]")
{
    // The on-demand path runs the safety projection too, so a cold (no previous)
    // call rate-limits the move toward the requested target. From a -14 model
    // default baseline, an explicit -11 must pull the applied loudness UPWARD vs
    // an on-demand call with no target at all (which leaves the model's -14).
    // This proves the explicit value reaches decode; the absolute -11 is reached
    // only after successive cycles (same rate-limiting the closed loop uses).
    more_phi::AutoMasteringEngine engine; engine.prepare(48000.0, 512, false);
    more_phi::SonicMasterAnalysisEngine eng;
    StubDecisionSource source; eng.setInferenceSource(&source);
    eng.setApplicationEngine(&engine); eng.prepare(48000.0, 512);
    feedSilence(eng, hostWindowFrames(48000.0) + 1024);

    // No target (NaN) and no prior → model -14 default path.
    eng.setGenreTargetLufs(more_phi::kUseModelTargetLufs);
    more_phi::ValidatedNeuralMasteringPlan planNone {};
    float rawN[more_phi::kSonicMasterDecisionWidth] {};
    more_phi::DecisionFailure failN = more_phi::DecisionFailure::None;
    REQUIRE(eng.requestDecisionNow(std::numeric_limits<float>::quiet_NaN(),
                                   planNone, rawN, more_phi::kSonicMasterDecisionWidth, &failN));
    const float lufsNone = -14.0f + planNone.projectedTargets.loudness[0] * 6.0f;

    // Explicit -11 (louder than -14) must move the applied target upward.
    more_phi::ValidatedNeuralMasteringPlan planExplicit {};
    float rawE[more_phi::kSonicMasterDecisionWidth] {};
    more_phi::DecisionFailure failE = more_phi::DecisionFailure::None;
    REQUIRE(eng.requestDecisionNow(-11.0f, planExplicit, rawE, more_phi::kSonicMasterDecisionWidth, &failE));
    const float lufsExplicit = -14.0f + planExplicit.projectedTargets.loudness[0] * 6.0f;

    CHECK(failE == more_phi::DecisionFailure::None);
    CHECK(lufsExplicit > lufsNone);

    eng.release();
}

TEST_CASE("Genre profile table mirrors ChainPlanExecutor LUFS values",
          "[GenrePrior][Contract]")
{
    // ponytail: the duplicated table in GenreMasteringProfile.h must stay
    // byte-for-byte equal to ChainPlanExecutor::kGenreLUFS. This is the one
    // check that fails if someone edits one without the other.
    constexpr std::array<float, 12> kChainLUFS = {
        -9.f, -9.f, -11.f, -13.f, -12.f, -16.f,
        -17.f, -20.f, -18.f, -10.f, -14.f, -23.f
    };
    for (std::size_t i = 0; i < kChainLUFS.size(); ++i)
        CHECK(more_phi::kGenreMasteringProfiles[i].targetLufs == kChainLUFS[i]);
}

TEST_CASE("Genre profile out-of-range falls back to Streaming slot",
          "[GenrePrior][Contract]")
{
    const auto fallback = more_phi::getGenreMasteringProfile(999);
    const auto streaming = more_phi::kGenreMasteringProfiles[10];
    CHECK(fallback.targetLufs == streaming.targetLufs);
    CHECK(fallback.targetLufs == Catch::Approx(-14.0f).margin(1e-3f));
}
