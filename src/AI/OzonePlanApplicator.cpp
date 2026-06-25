/*
 * More-Phi — AI/OzonePlanApplicator.cpp
 */
#include "OzonePlanApplicator.h"
#include "Plugin/PluginProcessor.h"
#include "Core/AutoMasteringEngine.h"   // AUDIT-FIX (Fix 2): ApplyVerification definition
#include <nlohmann/json.hpp>
#include <cmath>
#include <algorithm>

namespace more_phi {

using json = nlohmann::json;

// AUDIT-FIX (Fix 2): base-class default — returns an empty verification so test
// stubs that don't override getLastVerification() still compile and report "no
// verification performed" rather than failing to link.
ApplyVerification OzonePlanApplicatorBase::getLastVerification() const noexcept
{
    return {};
}

ApplyVerification OzonePlanApplicator::getLastVerification() const noexcept
{
    // AUDIT-FIX (Fix 2): readback verification. For each param enqueued in the last
    // apply(), read the hosted plugin's current normalized value back and compare to
    // the requested (post-snap) value. Discrete-aware tolerance mirrors
    // MCPToolHandler::classifyVerification: discrete/boolean params get
    // max(0.5/numSteps, 0.001) so a legitimate step-snap isn't flagged as drift;
    // continuous params use 0.001. Drift vs mismatch is distinguished so callers can
    // tell "snapped to a valid step" (driftedDiscrete, usually fine) from "the write
    // didn't land" (mismatched, a real problem).
    ApplyVerification v;
    v.requested = lastBreakdown_.enqueued + lastBreakdown_.unmapped
                + lastBreakdown_.ambiguous + lastBreakdown_.skipped;
    v.enqueued  = lastBreakdown_.enqueued;
    v.unmapped  = lastBreakdown_.unmapped;
    v.ambiguous = lastBreakdown_.ambiguous;

    auto& bridge = processor_.getParameterBridge();
    for (const auto& p : lastBreakdown_.applied)
    {
        const float actual = bridge.getParameterNormalized(p.index);
        const int numSteps = bridge.getParameterNumSteps(p.index);
        const bool discrete = bridge.isDiscrete(p.index) || bridge.isBoolean(p.index);
        // Discrete-aware tolerance: half a step (so adjacent-step snaps pass), with
        // a 0.001 floor for booleans / numSteps<=1.
        const float tol = discrete
            ? std::max(0.5f / static_cast<float>(std::max(1, numSteps)), 0.001f)
            : 0.001f;
        const float delta = std::abs(actual - p.requestedNormalized);
        if (delta <= tol)
        {
            ++v.verified;
        }
        else if (discrete)
        {
            ++v.driftedDiscrete;
        }
        else
        {
            ++v.mismatched;
        }
    }
    return v;
}

OzonePlanApplicator::OzonePlanApplicator(MorePhiProcessor& processor,
                                         const OzoneParameterMap& map) noexcept
    : processor_(processor), map_(map)
{
}

int OzonePlanApplicator::apply(const MultiEffectPlan& plan)
{
    if (!plan.valid)
        return 0;

    // AUDIT-FIX-5: fail loud. If no parameter has been audited/mapped yet, log a
    // warning so the silent no-op cannot masquerade as a successful apply. Until
    // audit_ozone_parameters(apply=true) runs against the hosted Ozone instance,
    // every plan field writes nothing — callers deserve to know.
    if (!map_.hasAnyMapping())
    {
        DBG(
            "OzonePlanApplicator: WARNING — parameter map is all-stubs; "
            "no Ozone parameters will be set. Run audit_ozone_parameters(apply=true) "
            "against the hosted plugin to populate the map.");
        lastAppliedCount_ = 0;
        lastBreakdown_ = {};   // AUDIT-FIX (Fix 6)
        return 0;
    }

    // AUDIT-FIX (Fix 6): reset the breakdown so it always reflects THIS call.
    lastBreakdown_ = OzoneApplyBreakdown{};

    int total = 0;
    total += applyEQ(plan);
    total += applyDynamics(plan);
    total += applyStereoImager(plan);
    total += applyMaximizer(plan);

    // P3.10 (AUDIT): close the plan with a transaction boundary so the audio
    // thread can confirm the full plan drained (getLastDrainedPlanId). Only emit
    // when at least one parameter was enqueued — a zero-param apply has nothing to
    // commit. The boundary is a no-op command (paramIndex=-1) and does not count
    // toward the returned total.
    if (total > 0)
    {
        const std::uint64_t planId = ++lastPlanId_;
        processor_.enqueuePlanBoundary(planId, MorePhiProcessor::ParameterEditSource::MCP);
    }

    lastAppliedCount_ = total;
    return total;
}

// ── EQ ───────────────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyEQ(const MultiEffectPlan& plan)
{
    if (plan.eqPrescriptionJSON.isEmpty())
        return 0;

    int count = 0;
    try
    {
        const auto j = json::parse(plan.eqPrescriptionJSON.toStdString());
        if (!j.contains("bands") || !j["bands"].is_array())
            return 0;

        const auto& bands = j["bands"];
        const int numBands = std::min(static_cast<int>(bands.size()),
                                      OzoneParameterMap::kEQBands);

        for (int i = 0; i < numBands; ++i)
        {
            const auto& b   = bands[i];
            const auto& bm  = map_.eq[i];

            const float freq    = b.value("freq", 1000.0f);
            const float gain    = b.value("gain",    0.0f);
            const float q       = b.value("Q",       1.0f);
            const auto  typeStr = juce::String(b.value("type", "peak"));

            count += enqueueIfMapped(bm.freqIdx,
                OzoneParameterMap::normalizeFreq(freq),
                "eq band " + juce::String(i + 1) + " frequency");
            count += enqueueIfMapped(bm.gainIdx,
                // P2.7 (AUDIT): pass the SAME ±AdaptiveEQ::kMaxGainDB range the
                // decode path uses (AutoMasteringEngine.cpp:469-472 and
                // buildBridgePlanFromNeural :640-643 decode eq[i] *
                // AdaptiveEQ::kMaxGainDB). normalizeGain() defaults to ±18 dB;
                // with kMaxGainDB == 12, a decoded +12 dB gain re-encoded over
                // ±18 dB arrived at the plugin at ~0.833 instead of 1.0 — a 1.5x
                // systematic under-gain across all 8 EQ bands. Centralizing on the
                // decode constant makes the round-trip identity.
                OzoneParameterMap::normalizeGain(gain,
                    -AdaptiveEQ::kMaxGainDB, AdaptiveEQ::kMaxGainDB),
                "eq band " + juce::String(i + 1) + " gain");
            count += enqueueIfMapped(bm.qIdx,
                // Q is linear in VST3 normalized space over [0.1, 8.0] — see
                // OzoneParameterMap::normalizeQ. Do NOT route through normalizeFreq
                // (log2); that distorted Q and used a range ([0.1, 20]) that
                // disagreed with PluginSemanticMapper's documented Q domain.
                OzoneParameterMap::normalizeQ(q),
                "eq band " + juce::String(i + 1) + " q");
            count += enqueueIfMapped(bm.typeIdx,
                OzoneParameterMap::encodeFilterType(typeStr),
                "eq band " + juce::String(i + 1) + " type");
            count += enqueueIfMapped(bm.enabledIdx, 1.0f,  // enable band
                "eq band " + juce::String(i + 1) + " enabled");
        }
    }
    catch (const std::exception&)
    {
        // Malformed JSON — skip EQ application
    }
    return count;
}

// ── Dynamics ─────────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyDynamics(const MultiEffectPlan& plan)
{
    const auto& dm = map_.dynamics;
    int count = 0;

    // compressionNeed [0..1] → threshold dBFS (lerp from gentle to aggressive)
    const float threshDB = kThresholdAtMinNeed
        + plan.compressionNeed * (kThresholdAtMaxNeed - kThresholdAtMinNeed);
    count += enqueueIfMapped(dm.thresholdIdx,
        OzoneParameterMap::normalizeThreshold(threshDB),
        "dynamics threshold");

    // compressionNeed [0..1] → ratio
    const float ratio = kRatioAtMinNeed
        + plan.compressionNeed * (kRatioAtMaxNeed - kRatioAtMinNeed);
    const float ratioNorm = std::clamp(
        (ratio - kOzoneRatioMin) / (kOzoneRatioMax - kOzoneRatioMin), 0.0f, 1.0f);
    count += enqueueIfMapped(dm.ratioIdx, ratioNorm, "dynamics ratio");

    // Fixed attack/release defaults (good mastering starting points)
    const float attackNorm = std::clamp(
        (kDefaultAttackMs - kOzoneAttackMin) / (kOzoneAttackMax - kOzoneAttackMin),
        0.0f, 1.0f);
    const float releaseNorm = std::clamp(
        (kDefaultReleaseMs - kOzoneReleaseMin) / (kOzoneReleaseMax - kOzoneReleaseMin),
        0.0f, 1.0f);
    count += enqueueIfMapped(dm.attackIdx,  attackNorm,  "dynamics attack");
    count += enqueueIfMapped(dm.releaseIdx, releaseNorm, "dynamics release");

    return count;
}

// ── Stereo Imager ─────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyStereoImager(const MultiEffectPlan& plan)
{
    int count = 0;
    // P2.6: expected names mirror the buildFromHostedPlugin band-order tokens.
    static constexpr const char* kImagerBandNames[4] = {
        "imager sub width", "imager low width",
        "imager mid width", "imager high width"
    };
    for (int i = 0; i < 4; ++i)
    {
        count += enqueueIfMapped(map_.imager.widthIdx[i],
            OzoneParameterMap::normalizeWidth(plan.widthCurve[i]),
            kImagerBandNames[i]);
    }
    return count;
}

// ── Maximizer ────────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyMaximizer(const MultiEffectPlan& plan)
{
    int count = 0;
    count += enqueueIfMapped(map_.maximizer.outputLevelIdx,
        OzoneParameterMap::normalizeLUFS(plan.targetLUFS),
        "maximizer output level");
    count += enqueueIfMapped(map_.maximizer.ceilingIdx,
        OzoneParameterMap::normalizeCeiling(plan.ceilingDBTP),
        "maximizer ceiling");
    return count;
}

// ── Private helpers ───────────────────────────────────────────────────────────

int OzonePlanApplicator::enqueueIfMapped(int idx, float normalizedValue)
{
    return enqueueIfMapped(idx, normalizedValue, /*expectedName=*/{});
}

int OzonePlanApplicator::enqueueIfMapped(int idx, float normalizedValue,
                                         const juce::String& expectedName)
{
    // AUDIT-FIX (Fix 6): classify the skip reason so applyValidatedPlan can tell
    // partial-apply from all-unmapped. idx == -2 (Fix 3, ambiguity) is counted as
    // ambiguous; idx == -1 is unmapped. Both still return 0 to preserve the int API.
    if (idx == -2)
    {
        ++lastBreakdown_.ambiguous;
        return 0;
    }
    if (idx < 0)
    {
        ++lastBreakdown_.unmapped;
        return 0;
    }
    // P2.6 (AUDIT): index-drift re-validation. The OzoneParameterMap is built once
    // against a hosted plugin's positional indices. If the user swaps plugins, the
    // stale index (e.g. EQ Band 1 Freq = 42) writes to whatever parameter sits at
    // position 42 in the NEW plugin — silently corrupting an unrelated control.
    // When the caller supplies the name the slot was mapped from, re-read the
    // hosted plugin's current parameter name at that index and bail (counted as
    // skipped) on mismatch. The MCP setParameter path is immune (it re-resolves
    // names live each call); this brings the neural path to the same safety level
    // without rebuilding the whole map on every write.
    if (expectedName.isNotEmpty())
    {
        const juce::String actualName = processor_.getParameterBridge().getParameterName(idx);
        if (! nameMatches(expectedName, actualName))
        {
            ++lastBreakdown_.skipped;
            DBG("OzonePlanApplicator: index drift — expected '"
                + expectedName + "' at index " + juce::String(idx)
                + " but hosted plugin reports '" + actualName
                + "'; skipping to avoid writing the wrong parameter. "
                  "Re-hosting the plugin or rebuilding the map is required.");
            return 0;
        }
    }
    // AUDIT-FIX (Fix 5): snap discrete/binary params to the nearest valid step
    // before enqueue. The raw enqueue path writes a continuous float regardless of
    // isDiscrete/isBoolean and relies on the hosted plugin to self-snap — which is
    // unreliable (e.g. enabledIdx ← 1.0f, typeIdx ← 0.25). Continuous params pass
    // through unchanged (snapNormalizedToStep is identity for non-discrete).
    const float snapped = processor_.getParameterBridge().snapNormalizedToStep(idx, normalizedValue);
    const bool ok = processor_.enqueueParameterSet(
        idx, snapped,
        MorePhiProcessor::ParameterEditSource::MCP,
        // P2.4 (AUDIT): hold the AI edit against morph. The neural/Ozone path
        // previously passed false, so a running morph block could overwrite the
        // recommended value before/while it drained — the edit vanished audibly.
        // The MCP setParameter path already holds (MCPToolHandler.cpp:3344) for
        // exactly this reason; the two AI entry paths now share the invariant.
        // Drain-delay risk under commandConsumerLock_ is bounded: the value is
        // retained in the ring buffer and the hold prevents morph clobbering it
        // while it waits.
        /*holdAgainstMorph=*/true);
    if (ok)
    {
        ++lastBreakdown_.enqueued;
        // AUDIT-FIX (Fix 2): record the (index, requested) pair for readback.
        // 'requested' is the post-snap value actually pushed to the queue.
        lastBreakdown_.applied.push_back({ idx, snapped });
        return 1;
    }
    ++lastBreakdown_.skipped;
    return 0;
}

// P2.6 (AUDIT): case-insensitive, trimming comparator for the index-drift check.
// The map stores names as built by buildFromHostedPlugin (lowercased substrings of
// the hosted plugin's own names), so a direct == would be too strict; a substring
// check in both directions tolerates the vendor's suffix/prefix variations while
// still catching a genuine mismatch (e.g. "EQ Band 1 Frequency" vs "Compressor Mix").
bool OzonePlanApplicator::nameMatches(const juce::String& expectedName,
                                      const juce::String& actualName) const noexcept
{
    if (expectedName.isEmpty() || actualName.isEmpty())
        return false;
    const juce::String e = expectedName.toLowerCase().trim();
    const juce::String a = actualName.toLowerCase().trim();
    // OzoneParameterMap::buildFromHostedPlugin keys on substrings ("frequency",
    // "gain", "bandwidth", "threshold", etc.). Accept if the expected token is
    // present in the actual name — robust to "EQ Band 1 Frequency (Hz)"-style
    // display decoration. Require BOTH the module token and (for EQ) the band
    // qualifier to match where present, by checking the expected string wholesale.
    return a.contains(e) || e.contains(a);
}

} // namespace more_phi
