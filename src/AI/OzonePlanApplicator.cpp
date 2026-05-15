/*
 * More-Phi — AI/OzonePlanApplicator.cpp
 */
#include "OzonePlanApplicator.h"
#include "Plugin/PluginProcessor.h"
#include <nlohmann/json.hpp>
#include <cmath>
#include <algorithm>

namespace more_phi {

using json = nlohmann::json;

OzonePlanApplicator::OzonePlanApplicator(MorePhiProcessor& processor,
                                         const OzoneParameterMap& map) noexcept
    : processor_(processor), map_(map)
{
}

int OzonePlanApplicator::apply(const MultiEffectPlan& plan)
{
    if (!plan.valid)
        return 0;

    int total = 0;
    total += applyEQ(plan);
    total += applyDynamics(plan);
    total += applyStereoImager(plan);
    total += applyMaximizer(plan);
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
                OzoneParameterMap::normalizeFreq(freq));
            count += enqueueIfMapped(bm.gainIdx,
                OzoneParameterMap::normalizeGain(gain));
            count += enqueueIfMapped(bm.qIdx,
                // Q typically [0.1..20] → [0..1] using log scale like freq
                OzoneParameterMap::normalizeFreq(q, 0.1f, 20.0f));
            count += enqueueIfMapped(bm.typeIdx,
                OzoneParameterMap::encodeFilterType(typeStr));
            count += enqueueIfMapped(bm.enabledIdx, 1.0f);  // enable band
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
        OzoneParameterMap::normalizeThreshold(threshDB));

    // compressionNeed [0..1] → ratio
    const float ratio = kRatioAtMinNeed
        + plan.compressionNeed * (kRatioAtMaxNeed - kRatioAtMinNeed);
    const float ratioNorm = std::clamp(
        (ratio - kOzoneRatioMin) / (kOzoneRatioMax - kOzoneRatioMin), 0.0f, 1.0f);
    count += enqueueIfMapped(dm.ratioIdx, ratioNorm);

    // Fixed attack/release defaults (good mastering starting points)
    const float attackNorm = std::clamp(
        (kDefaultAttackMs - kOzoneAttackMin) / (kOzoneAttackMax - kOzoneAttackMin),
        0.0f, 1.0f);
    const float releaseNorm = std::clamp(
        (kDefaultReleaseMs - kOzoneReleaseMin) / (kOzoneReleaseMax - kOzoneReleaseMin),
        0.0f, 1.0f);
    count += enqueueIfMapped(dm.attackIdx,  attackNorm);
    count += enqueueIfMapped(dm.releaseIdx, releaseNorm);

    return count;
}

// ── Stereo Imager ─────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyStereoImager(const MultiEffectPlan& plan)
{
    int count = 0;
    for (int i = 0; i < 4; ++i)
    {
        count += enqueueIfMapped(map_.imager.widthIdx[i],
            OzoneParameterMap::normalizeWidth(plan.widthCurve[i]));
    }
    return count;
}

// ── Maximizer ────────────────────────────────────────────────────────────────

int OzonePlanApplicator::applyMaximizer(const MultiEffectPlan& plan)
{
    int count = 0;
    count += enqueueIfMapped(map_.maximizer.outputLevelIdx,
        OzoneParameterMap::normalizeLUFS(plan.targetLUFS));
    count += enqueueIfMapped(map_.maximizer.ceilingIdx,
        OzoneParameterMap::normalizeCeiling(plan.ceilingDBTP));
    return count;
}

// ── Private helpers ───────────────────────────────────────────────────────────

int OzonePlanApplicator::enqueueIfMapped(int idx, float normalizedValue)
{
    if (idx < 0)
        return 0;
    const bool ok = processor_.enqueueParameterSet(
        idx, normalizedValue,
        MorePhiProcessor::ParameterEditSource::MCP,
        /*holdAgainstMorph=*/false);
    return ok ? 1 : 0;
}

} // namespace more_phi
