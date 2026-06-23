/*
 * More-Phi — Core/MultibandDynamicsProcessor.cpp
 */
#include "MultibandDynamicsProcessor.h"
#include <cmath>
#include <algorithm>

namespace more_phi {

struct DefaultBandParams {
    float threshDB, ratio, attackMs, releaseMs;
};

static const DefaultBandParams kDefaults[MultibandDynamicsProcessor::kNumBands] = {
    { -18.f, 1.5f,  50.f, 200.f },   // 0: Sub
    { -20.f, 2.5f,  15.f, 150.f },   // 1: Low
    { -22.f, 3.0f,   8.f, 120.f },   // 2: Mid
    { -18.f, 2.0f,   3.f,  80.f },   // 3: High
};

MultibandDynamicsProcessor::MultibandDynamicsProcessor()
{
    applyDefaults();
    for (auto& gr : grDB_)
        gr.store(0.f, std::memory_order_relaxed);
}

void MultibandDynamicsProcessor::applyDefaults() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];
        bd.thresholdLinear.store(std::pow(10.f, kDefaults[b].threshDB / 20.f), std::memory_order_relaxed);
        bd.ratio.store(kDefaults[b].ratio, std::memory_order_relaxed);
        bd.attackMs.store(kDefaults[b].attackMs, std::memory_order_relaxed);
        bd.releaseMs.store(kDefaults[b].releaseMs, std::memory_order_relaxed);
        bd.makeupLinear.store(1.0f, std::memory_order_relaxed);
        bd.kneeDB.store(2.0f, std::memory_order_relaxed);
        bd.enabled.store(true, std::memory_order_relaxed);
    }
}

void MultibandDynamicsProcessor::setBandParams(int band, const BandParams& p) noexcept
{
    if (band < 0 || band >= kNumBands) return;
    auto& bd = bands_[band];
    bd.thresholdLinear.store(std::pow(10.f, p.thresholdDB / 20.f), std::memory_order_relaxed);
    bd.ratio.store(p.ratio, std::memory_order_relaxed);
    bd.attackMs.store(p.attackMs, std::memory_order_relaxed);
    bd.releaseMs.store(p.releaseMs, std::memory_order_relaxed);
    bd.makeupLinear.store(std::pow(10.f, p.makeupDB / 20.f), std::memory_order_relaxed);
    bd.kneeDB.store(p.kneeDB, std::memory_order_relaxed);

    // AUDIT-FIX: Pre-compute per-sample attack/release coefficients so
    // processBlock() never calls std::exp on the audio thread. Follows the
    // same pattern as EnvelopeFollower's pre-computed per-block coefficients.
    const float sr = static_cast<float>(sampleRate_);
    if (sr > 0.0f)
    {
        bd.attackCoeffPerSample.store(
            std::exp(-1.0f / (p.attackMs * 0.001f * sr)),
            std::memory_order_relaxed);
        bd.releaseCoeffPerSample.store(
            std::exp(-1.0f / (p.releaseMs * 0.001f * sr)),
            std::memory_order_relaxed);
    }

    // Recompute envelope follower coefficients now
    for (int ch = 0; ch < kMaxChannels; ++ch)
    {
        bd.followers[ch].setAttack(p.attackMs);
        bd.followers[ch].setRelease(p.releaseMs);
    }
}

MultibandDynamicsProcessor::BandParams MultibandDynamicsProcessor::getBandParams(int band) const noexcept
{
    if (band < 0 || band >= kNumBands) return {};
    const auto& bd = bands_[band];
    BandParams p;
    const float tl = bd.thresholdLinear.load(std::memory_order_relaxed);
    p.thresholdDB = (tl > 1e-12f) ? 20.f * std::log10(tl) : -120.f;
    p.ratio       = bd.ratio.load(std::memory_order_relaxed);
    p.attackMs    = bd.attackMs.load(std::memory_order_relaxed);
    p.releaseMs   = bd.releaseMs.load(std::memory_order_relaxed);
    const float ml = bd.makeupLinear.load(std::memory_order_relaxed);
    p.makeupDB    = (ml > 1e-12f) ? 20.f * std::log10(ml) : 0.f;
    p.kneeDB      = bd.kneeDB.load(std::memory_order_relaxed);
    return p;
}

void MultibandDynamicsProcessor::setBandEnabled(int band, bool enabled) noexcept
{
    if (band >= 0 && band < kNumBands)
        bands_[band].enabled.store(enabled, std::memory_order_relaxed);
}

void MultibandDynamicsProcessor::prepare(double sampleRate, int /*maxBlockSize*/)
{
    sampleRate_ = sampleRate;
    const float sr = static_cast<float>(sampleRate);
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];

        // AUDIT-FIX: Pre-compute per-sample attack/release coefficients so
        // processBlock() reads atomics instead of calling std::exp.
        bd.attackCoeffPerSample.store(
            std::exp(-1.0f / (bd.attackMs.load(std::memory_order_relaxed) * 0.001f * sr)),
            std::memory_order_relaxed);
        bd.releaseCoeffPerSample.store(
            std::exp(-1.0f / (bd.releaseMs.load(std::memory_order_relaxed) * 0.001f * sr)),
            std::memory_order_relaxed);

        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            bd.followers[ch].prepare(sampleRate);
            bd.followers[ch].setAttack(bd.attackMs.load(std::memory_order_relaxed));
            bd.followers[ch].setRelease(bd.releaseMs.load(std::memory_order_relaxed));
            bd.gainSmoothed[ch] = 1.0f;
        }
    }
}

void MultibandDynamicsProcessor::reset() noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];
        for (int ch = 0; ch < kMaxChannels; ++ch)
        {
            bd.followers[ch].reset();
            bd.gainSmoothed[ch] = 1.0f;
        }
        bd.envLinked = 0.0f;  // MULTIBAND-2/3
        grDB_[b].store(0.f, std::memory_order_relaxed);
    }
}

float MultibandDynamicsProcessor::computeCompressorGain(float rmsLinear,
                                                         float threshLinear,
                                                         float ratio,
                                                         float kneeDB,
                                                         float makeup) const noexcept
{
    // W1 FIX: replace std::log10 / std::pow(10,...) (per-sample on the audio
    // thread, ~4 bands × N samples) with std::log / std::exp using the
    // identities log10(x) = log(x)/ln10 and 10^(y) = exp(y*ln10).
    // expf/logf are measurably faster than the base-10 variants on the common
    // libm paths and avoid the internal table lookups the base-10 versions do.
    // Math is preserved within float precision (same gain curve).
    constexpr float kLn10 = 2.30258509299404568402f;
    constexpr float kDbToLog = kLn10 / 20.0f;   // 20*log10(x) == log(x)/kDbToLogInv; here: dB->natural log
    const float rmsLogN = (rmsLinear    > 1e-12f) ? std::log(rmsLinear)    : (-120.f * kDbToLog);
    const float thrLogN = (threshLinear > 1e-12f) ? std::log(threshLinear) : (-120.f * kDbToLog);
    // excess in dB-equivalent natural-log units: (dB difference) * ln10/20.
    const float excessLogN = (rmsLogN - thrLogN) / kDbToLog;   // back to dB for the knee math
    const float kneeH  = kneeDB * 0.5f;

    float gainDB = 0.f;
    if (excessLogN <= -kneeH)
    {
        gainDB = 0.f;  // below knee: no reduction
    }
    else if (excessLogN < kneeH)
    {
        // Soft-knee interpolation (quadratic blend into compression)
        gainDB = (1.f / ratio - 1.f) * (excessLogN + kneeH) * (excessLogN + kneeH) / (2.f * kneeDB);
    }
    else
    {
        gainDB = excessLogN / ratio - excessLogN;  // = excess * (1/ratio - 1)
    }

    // makeup * 10^(gainDB/20) == makeup * exp(gainDB * ln10/20)
    return makeup * std::exp(gainDB * kDbToLog);
}

void MultibandDynamicsProcessor::processBlock(juce::AudioBuffer<float> bands[kNumBands]) noexcept
{
    // Self-contained denormal guard: the per-sample envelope can denormalize on
    // silence. The DAW audio path is also covered by PluginProcessor's guard, but
    // the offline render / CLI path may not be, so flush here to be safe.
    const juce::ScopedNoDenormals noDenormals;

    for (int b = 0; b < kNumBands; ++b)
    {
        auto& bd = bands_[b];
        if (!bd.enabled.load(std::memory_order_relaxed))
        {
            grDB_[b].store(0.0f, std::memory_order_relaxed);  // don't show stale GR for a bypassed band
            continue;
        }

        const float threshLinear = bd.thresholdLinear.load(std::memory_order_relaxed);
        const float ratio        = bd.ratio.load(std::memory_order_relaxed);
        const float kneeDB       = bd.kneeDB.load(std::memory_order_relaxed);
        const float makeup       = bd.makeupLinear.load(std::memory_order_relaxed);

        juce::AudioBuffer<float>& buf = bands[b];
        const int ns  = buf.getNumSamples();
        const int nch = std::min(buf.getNumChannels(), kMaxChannels);
        if (ns == 0) continue;

        // MULTIBAND-2/3 FIX: stereo-LINKED, per-SAMPLE detection. Previously
        // each channel had its own block-RMS detector + gain, so a hard-panned
        // transient caused L/R gain to diverge (image wander) and the single
        // block-RMS target could not track intra-block transients. Now one
        // linked peak detector feeds one gain applied equally to both channels,
        // updated every sample (which also makes the attack/release coefficients
        // — already per-sample — behave with the correct time constant).
        //
        // AUDIT-FIX: attack/release coefficients are pre-computed in setBandParams()
        // and prepare() (stored as atomics). Reading them here avoids four std::exp
        // calls per block on the audio thread.
        const float attackCoeff  = bd.attackCoeffPerSample.load(std::memory_order_relaxed);
        const float releaseCoeff = bd.releaseCoeffPerSample.load(std::memory_order_relaxed);

        float* L = (nch >= 1) ? buf.getWritePointer(0) : nullptr;
        float* R = (nch >= 2) ? buf.getWritePointer(1) : nullptr;

        float worstGain = 1.0f;  // MULTIBAND-4: pure compression gain (no makeup) for the meter

        for (int i = 0; i < ns; ++i)
        {
            // Linked peak detector across active channels
            float det = 0.0f;
            if (L != nullptr) det = std::abs(L[i]);
            if (R != nullptr) det = std::max(det, std::abs(R[i]));

            // Per-sample envelope (attack/release on the detector level)
            const float coeff = (det > bd.envLinked) ? attackCoeff : releaseCoeff;
            bd.envLinked = bd.envLinked * coeff + det * (1.0f - coeff);

            // Compression gain from the smoothed level (makeup applied separately
            // so the GR meter reads pure compression).
            const float gain = computeCompressorGain(bd.envLinked, threshLinear, ratio, kneeDB, 1.0f);
            if (gain < worstGain) worstGain = gain;

            const float applied = gain * makeup;
            if (L != nullptr) L[i] *= applied;
            if (R != nullptr) R[i] *= applied;
        }

        // MULTIBAND-4 FIX: GR meter reports pure compression gain, independent of makeup.
        const float grDB = (worstGain < 1.f && worstGain > 1e-12f)
                           ? 20.f * std::log10(worstGain) : 0.f;
        grDB_[b].store(grDB, std::memory_order_relaxed);
    }
}

} // namespace more_phi
