/*
 * More-Phi — tests/Unit/TestNeuralPlanSerialization.cpp
 *
 * Covers AUDIT F1 (2026-06-30): AutoMasteringEngine::serializeLastPlan writes a
 * `version` attribute (kNeuralMasteringPlanSchemaVersion) on the MASTERING_PLAN
 * element, and restoreLastPlan MUST read + gate on it. Before the fix the
 * attribute was write-only — a size-preserving layout change in
 * ValidatedNeuralMasteringPlan would silently re-interpret the bytes and
 * corrupt the restored plan.
 *
 * This suite pins the schema contract:
 *   - round-trip of a well-formed plan (version present + correct, size exact,
 *     valid flag set) succeeds and arms hasLastSafeNeuralMasteringPlan.
 *   - every structural deviation is REJECTED (returns false) rather than
 *     best-effort loaded: missing element, empty data, corrupt base64, truncated
 *     payload (size mismatch), wrong version attribute, missing version
 *     attribute, and valid==false inside the decoded plan.
 *
 * Uses AutoMasteringEngine directly with a mock OzonePlanApplicatorBase
 * (pattern from TestNeuralPlanVerification.cpp) — no real MorePhiProcessor.
 */
#include "Core/AutoMasteringEngine.h"
#include "AI/ChainPlanExecutor.h"
#include "AI/OzonePlanApplicator.h"
#include "Core/NeuralMasteringTypes.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

namespace {

// Minimal applicator stub — applyValidatedPlan only needs it to exist on the
// chain planner so the bridge path doesn't no-op the plan arming. We never
// inspect its return; the test cares about lastSafeNeuralPlan_ being armed.
class CountingMockApplicator : public more_phi::OzonePlanApplicatorBase
{
public:
    int apply(const more_phi::MultiEffectPlan&) override { return 1; }
    int getLastAppliedCount() const noexcept override { return 1; }
    more_phi::OzoneApplyBreakdown getLastApplyBreakdown() const noexcept override { return {}; }
    more_phi::ApplyVerification getLastVerification() const noexcept override { return {}; }
    std::uint64_t getLastSubmittedPlanId() const noexcept override { return 0; }
};

// Build a minimal but valid neural plan that arms hasLastSafeNeuralPlan_ when
// passed to applyValidatedPlan. Mirrors TestNeuralPlanVerification::makeMinimalPlan.
more_phi::ValidatedNeuralMasteringPlan makeMinimalPlan()
{
    more_phi::ValidatedNeuralMasteringPlan plan{};
    plan.valid = true;
    plan.fallbackMode = more_phi::NeuralMasteringFallbackMode::None;
    plan.projected = true;
    plan.projectedTargets.loudness[0] = 0.5f;
    plan.appliedMask.loudness = true;
    return plan;
}

// Engine fixture with a mock applicator installed, ready to arm + serialize a
// plan. Returns the engine by value-move-friendly reference via the lambda.
struct ArmedEngine
{
    more_phi::AutoMasteringEngine engine;
    CountingMockApplicator applicator;
    explicit ArmedEngine(double sampleRate = 48000.0)
        : engine{}
    {
        engine.prepare(sampleRate, 512, /*startIntelligence=*/false);
        engine.getChainPlanner().setOzonePlanApplicator(&applicator);
    }
    ~ArmedEngine()
    {
        engine.getChainPlanner().clearOzonePlanApplicator();
    }
};

} // anonymous namespace

// ── Round-trip: the happy path must still work after the version gate is added ─

TEST_CASE("serializeLastPlan/restoreLastPlan round-trip a valid plan",
          "[state][neural][audit-f1]")
{
    ArmedEngine env;
    REQUIRE(env.engine.applyValidatedPlan(makeMinimalPlan()));
    REQUIRE(env.engine.hasLastSafeNeuralMasteringPlan());

    juce::XmlElement parent("ROOT");
    env.engine.serializeLastPlan(parent);

    // The element + all three attributes must be present.
    const auto* el = parent.getChildByName("MASTERING_PLAN");
    REQUIRE(el != nullptr);
    REQUIRE(el->getStringAttribute("data").isNotEmpty());
    REQUIRE(el->getBoolAttribute("hasPlan", false));
    REQUIRE(el->getIntAttribute("version", 0)
            == static_cast<int>(more_phi::kNeuralMasteringPlanSchemaVersion));

    // Restore into a fresh engine (no plan armed yet) must succeed and re-arm.
    ArmedEngine env2;
    REQUIRE_FALSE(env2.engine.hasLastSafeNeuralMasteringPlan());
    REQUIRE(env2.engine.restoreLastPlan(parent));
    REQUIRE(env2.engine.hasLastSafeNeuralMasteringPlan());

    // The restored plan carries the loudness target we set.
    const auto& restored = env2.engine.getLastSafeNeuralMasteringPlan();
    CHECK(restored.valid);
    CHECK(restored.projectedTargets.loudness[0] == Catch::Approx(0.5f).margin(1e-6f));
}

// ── Rejections: every malformed input returns false, never best-effort loads ──

TEST_CASE("restoreLastPlan rejects a missing MASTERING_PLAN element",
          "[state][neural][audit-f1]")
{
    ArmedEngine env;
    juce::XmlElement parent("ROOT");   // no child at all
    CHECK_FALSE(env.engine.restoreLastPlan(parent));
    CHECK_FALSE(env.engine.hasLastSafeNeuralMasteringPlan());
}

TEST_CASE("restoreLastPlan rejects empty data attribute",
          "[state][neural][audit-f1]")
{
    ArmedEngine env;
    juce::XmlElement parent("ROOT");
    auto* el = parent.createNewChildElement("MASTERING_PLAN");
    el->setAttribute("version", static_cast<int>(more_phi::kNeuralMasteringPlanSchemaVersion));
    el->setAttribute("data", "");   // present but empty
    CHECK_FALSE(env.engine.restoreLastPlan(parent));
}

TEST_CASE("restoreLastPlan rejects corrupt (non-base64) data",
          "[state][neural][audit-f1]")
{
    ArmedEngine env;
    juce::XmlElement parent("ROOT");
    auto* el = parent.createNewChildElement("MASTERING_PLAN");
    el->setAttribute("version", static_cast<int>(more_phi::kNeuralMasteringPlanSchemaVersion));
    // "!!!!!" is not valid base64 payload for JUCE's decoder.
    el->setAttribute("data", "!!!!!not-base64!!!!!");
    CHECK_FALSE(env.engine.restoreLastPlan(parent));
}

TEST_CASE("restoreLastPlan rejects a truncated payload (size mismatch)",
          "[state][neural][audit-f1]")
{
    ArmedEngine env;
    juce::XmlElement parent("ROOT");
    auto* el = parent.createNewChildElement("MASTERING_PLAN");
    el->setAttribute("version", static_cast<int>(more_phi::kNeuralMasteringPlanSchemaVersion));
    // Encode a too-small buffer — decoded size != sizeof(ValidatedNeuralMasteringPlan).
    const char stub[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    el->setAttribute("data", juce::Base64::toBase64(stub, sizeof(stub)));
    CHECK_FALSE(env.engine.restoreLastPlan(parent));
}

// ── The core F1 case: a present-but-WRONG version must be rejected ────────────

TEST_CASE("restoreLastPlan rejects a mismatched schema version (F1 core)",
          "[state][neural][audit-f1]")
{
    // First build a genuinely valid blob (correct size, valid==true) so the
    // only thing wrong is the version attribute.
    ArmedEngine env;
    REQUIRE(env.engine.applyValidatedPlan(makeMinimalPlan()));
    juce::XmlElement parent("ROOT");
    env.engine.serializeLastPlan(parent);
    auto* el = parent.getChildByName("MASTERING_PLAN");
    REQUIRE(el != nullptr);
    const juce::String goodData = el->getStringAttribute("data");
    REQUIRE(goodData.isNotEmpty());

    // Mutate the version to a future/incompatible value. The size + valid checks
    // would PASS (the blob is well-formed), so only the version gate catches it.
    // Before F1 this plan would have been restored; now it must be rejected.
    el->setAttribute("version", 999);

    ArmedEngine env2;
    CHECK_FALSE(env2.engine.restoreLastPlan(parent));
    CHECK_FALSE(env2.engine.hasLastSafeNeuralMasteringPlan());
}

TEST_CASE("restoreLastPlan rejects a missing version attribute (legacy/malformed)",
          "[state][neural][audit-f1]")
{
    ArmedEngine env;
    REQUIRE(env.engine.applyValidatedPlan(makeMinimalPlan()));
    juce::XmlElement parent("ROOT");
    env.engine.serializeLastPlan(parent);
    auto* el = parent.getChildByName("MASTERING_PLAN");
    REQUIRE(el != nullptr);

    // Strip the version attribute entirely (simulates hand-edited or pre-version
    // element). getIntAttribute defaults to 0 → treated as unknown schema → reject.
    el->removeAttribute("version");

    ArmedEngine env2;
    CHECK_FALSE(env2.engine.restoreLastPlan(parent));
}

// ── A decoded plan with valid==false must be rejected (pre-existing guard) ────

TEST_CASE("restoreLastPlan rejects a decoded plan with valid==false",
          "[state][neural][audit-f1]")
{
    // Hand-build a plan blob whose bytes decode to the right size but with
    // valid==false. We serialize a real plan, then flip the `valid` byte offset
    // inside the decoded struct. Cheaper approach: make a plan with valid=false.
    more_phi::ValidatedNeuralMasteringPlan badPlan = makeMinimalPlan();
    badPlan.valid = false;

    juce::XmlElement parent("ROOT");
    auto* el = parent.createNewChildElement("MASTERING_PLAN");
    el->setAttribute("version", static_cast<int>(more_phi::kNeuralMasteringPlanSchemaVersion));
    const auto* raw = reinterpret_cast<const char*>(&badPlan);
    el->setAttribute("data",
        juce::Base64::toBase64(raw, static_cast<int>(sizeof(badPlan))));

    ArmedEngine env;
    CHECK_FALSE(env.engine.restoreLastPlan(parent));
    CHECK_FALSE(env.engine.hasLastSafeNeuralMasteringPlan());
}

// ── serializeLastPlan is a no-op when no plan is armed (must not emit element) ─

TEST_CASE("serializeLastPlan emits nothing when no plan is armed",
          "[state][neural][audit-f1]")
{
    ArmedEngine env;
    REQUIRE_FALSE(env.engine.hasLastSafeNeuralMasteringPlan());

    juce::XmlElement parent("ROOT");
    env.engine.serializeLastPlan(parent);
    CHECK(parent.getChildByName("MASTERING_PLAN") == nullptr);
}
