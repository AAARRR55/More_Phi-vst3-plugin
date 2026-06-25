/*
 * More-Phi — tests/Unit/TestNeuralPlanVerification.cpp
 *
 * Covers the audit remediations on the neural mastering → hosted-plugin path:
 *   P1.2  AutoMasteringEngine::applyValidatedPlan consults the applicator's
 *         read-back verification (getLastApplyVerification / lastApplyWasPartial)
 *   P1.2  ChainPlanExecutor::getLastOzoneVerification forwards to the applicator
 *   P2.7  EQ gain round-trip uses the decode range (±AdaptiveEQ::kMaxGainDB, 12),
 *         not the old default ±18 — so a decoded +12 dB gain re-encodes to 1.0
 *
 * Uses AutoMasteringEngine directly with a mock OzonePlanApplicatorBase, mirroring
 * the BridgeMockApplicator pattern in TestMorePhiIpcIntegration.cpp — no real
 * MorePhiProcessor is required.
 */
#include "Core/AutoMasteringEngine.h"
#include "Core/AdaptiveEQ.h"
#include "AI/ChainPlanExecutor.h"
#include "AI/OzonePlanApplicator.h"
#include "AI/OzoneParameterMap.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

namespace {

// Stub that records the last plan and lets the test dictate the verification +
// breakdown the applicator "reports", so we can assert applyValidatedPlan wires
// them through to the engine's getters (the P1.2 gap: previously built but never
// consulted).
struct VerificationStub
{
    int callCount = 0;
    more_phi::MultiEffectPlan lastPlan;
    int returnCount = 1;   // number of params "enqueued" (drives ozoneApplied)

    more_phi::OzoneApplyBreakdown breakdown{};
    more_phi::ApplyVerification verification{};
};

class VerificationMockApplicator : public more_phi::OzonePlanApplicatorBase
{
public:
    explicit VerificationMockApplicator(VerificationStub& stub) : stub_(stub) {}

    int apply(const more_phi::MultiEffectPlan& plan) override
    {
        ++stub_.callCount;
        stub_.lastPlan = plan;
        return stub_.returnCount;
    }
    int getLastAppliedCount() const noexcept override { return stub_.returnCount; }
    more_phi::OzoneApplyBreakdown getLastApplyBreakdown() const noexcept override { return stub_.breakdown; }
    more_phi::ApplyVerification getLastVerification() const noexcept override { return stub_.verification; }

private:
    VerificationStub& stub_;
};

// Build a minimal but valid neural plan that reaches the bridge path.
more_phi::ValidatedNeuralMasteringPlan makeMinimalPlan()
{
    more_phi::ValidatedNeuralMasteringPlan plan{};
    plan.valid = true;
    plan.fallbackMode = more_phi::NeuralMasteringFallbackMode::None;
    plan.projected = true;
    plan.projectedTargets.loudness[0] = 0.5f;   // -14 + 0.5*6 = -11 LUFS
    plan.appliedMask.loudness = true;
    return plan;
}

} // anonymous namespace

// ── P1.2: getLastOzoneVerification forwarder ──────────────────────────────────

TEST_CASE("ChainPlanExecutor forwards getLastOzoneVerification from applicator",
          "[neural][verification][p1.2]")
{
    more_phi::ChainPlanExecutor executor;

    // No applicator registered → empty verification.
    auto empty = executor.getLastOzoneVerification();
    CHECK(empty.verified == 0);
    CHECK(empty.enqueued == 0);

    VerificationStub stub;
    stub.verification.requested = 4;
    stub.verification.enqueued  = 4;
    stub.verification.verified  = 3;
    stub.verification.mismatched = 1;

    VerificationMockApplicator mock(stub);
    executor.setOzonePlanApplicator(&mock);

    const auto fwd = executor.getLastOzoneVerification();
    CHECK(fwd.requested  == 4);
    CHECK(fwd.enqueued   == 4);
    CHECK(fwd.verified   == 3);
    CHECK(fwd.mismatched == 1);
    CHECK(fwd.verifiedFraction() == Catch::Approx(0.75f).margin(1e-4f));

    executor.clearOzonePlanApplicator();
}

// ── P1.2: applyValidatedPlan populates lastApplyVerification / lastApplyWasPartial ─

TEST_CASE("applyValidatedPlan reflects applicator read-back in engine getters",
          "[neural][verification][p1.2]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);

    VerificationStub stub;
    stub.returnCount = 4;
    stub.verification.requested = 4;
    stub.verification.enqueued  = 4;
    stub.verification.verified  = 4;   // all verified → not partial

    VerificationMockApplicator mock(stub);
    engine.getChainPlanner().setOzonePlanApplicator(&mock);

    REQUIRE(engine.applyValidatedPlan(makeMinimalPlan()));

    // The verification reported by the applicator must surface on the engine.
    const auto v = engine.getLastApplyVerification();
    CHECK(v.requested == 4);
    CHECK(v.enqueued  == 4);
    CHECK(v.verified  == 4);
    CHECK(v.verifiedFraction() == Catch::Approx(1.0f).margin(1e-4f));
    CHECK_FALSE(engine.lastApplyWasPartial());

    engine.getChainPlanner().clearOzonePlanApplicator();
}

TEST_CASE("applyValidatedPlan flags partial when read-back verifies <80%",
          "[neural][verification][p1.2]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);

    VerificationStub stub;
    stub.returnCount = 10;
    // Breakdown: 10 requested, 10 enqueued.
    stub.breakdown.enqueued = 10;
    // Verification: only 5 of 10 read back within tolerance → 50% < 80% → partial.
    stub.verification.requested = 10;
    stub.verification.enqueued  = 10;
    stub.verification.verified  = 5;
    stub.verification.mismatched = 5;

    VerificationMockApplicator mock(stub);
    engine.getChainPlanner().setOzonePlanApplicator(&mock);

    REQUIRE(engine.applyValidatedPlan(makeMinimalPlan()));
    CHECK(engine.lastApplyWasPartial());
    CHECK(engine.getLastApplyVerification().verified == 5);

    engine.getChainPlanner().clearOzonePlanApplicator();
}

TEST_CASE("applyValidatedPlan flags partial when enqueue shortfall <80%",
          "[neural][verification][p1.2]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, /*startIntelligence=*/false);

    VerificationStub stub;
    stub.returnCount = 3;   // only 3 of 10 slots enqueued
    // Breakdown: 10 total requested (3 enqueued + 7 unmapped).
    stub.breakdown.enqueued = 3;
    stub.breakdown.unmapped = 7;
    stub.verification.requested = 10;
    stub.verification.enqueued  = 3;
    stub.verification.verified  = 3;   // all 3 that enqueued verified OK

    VerificationMockApplicator mock(stub);
    engine.getChainPlanner().setOzonePlanApplicator(&mock);

    REQUIRE(engine.applyValidatedPlan(makeMinimalPlan()));
    // Enqueue shortfall (3/10 = 30% < 80%) drives partial even though the 3 that
    // landed all verified.
    CHECK(engine.lastApplyWasPartial());

    engine.getChainPlanner().clearOzonePlanApplicator();
}

// ── P2.7: EQ gain round-trip uses ±AdaptiveEQ::kMaxGainDB (12), not ±18 ────────

TEST_CASE("EQ gain round-trip is identity over the decode range (±kMaxGainDB)",
          "[neural][verification][p2.7]")
{
    // The neural decode path produces gain = eq[i] * AdaptiveEQ::kMaxGainDB.
    // OzonePlanApplicator::applyEQ must re-encode with the SAME range so the
    // hosted plugin receives the intended dB. Previously normalizeGain defaulted
    // to ±18, under-gaining a decoded +12 dB to ~0.833 instead of 1.0.
    using more_phi::AdaptiveEQ;
    using more_phi::OzoneParameterMap;

    REQUIRE(AdaptiveEQ::kMaxGainDB == Catch::Approx(12.0f).margin(1e-3f));

    // +kMaxGainDB decoded → must re-encode to 1.0 over the decode range.
    const float plusFull = OzoneParameterMap::normalizeGain(
        AdaptiveEQ::kMaxGainDB, -AdaptiveEQ::kMaxGainDB, AdaptiveEQ::kMaxGainDB);
    CHECK(plusFull == Catch::Approx(1.0f).margin(1e-5f));

    // -kMaxGainDB → 0.0.
    const float minusFull = OzoneParameterMap::normalizeGain(
        -AdaptiveEQ::kMaxGainDB, -AdaptiveEQ::kMaxGainDB, AdaptiveEQ::kMaxGainDB);
    CHECK(minusFull == Catch::Approx(0.0f).margin(1e-5f));

    // 0 dB → 0.5 (midpoint of the decode range).
    const float zero = OzoneParameterMap::normalizeGain(
        0.0f, -AdaptiveEQ::kMaxGainDB, AdaptiveEQ::kMaxGainDB);
    CHECK(zero == Catch::Approx(0.5f).margin(1e-5f));

    // Regression: the OLD default (±18) would encode +12 dB as 0.8333, not 1.0.
    const float oldBuggy = OzoneParameterMap::normalizeGain(AdaptiveEQ::kMaxGainDB);  // default ±18
    CHECK(oldBuggy == Catch::Approx(12.0f / 36.0f).margin(1e-5f));   // 0.8333
    CHECK_FALSE(oldBuggy == Catch::Approx(1.0f).margin(1e-2f));
}
