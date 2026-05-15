/*
 * More-Phi — Core/AutoMasteringEngine.cpp
 */
#include "AutoMasteringEngine.h"
#include <algorithm>
#include <cmath>

namespace more_phi {

AutoMasteringEngine::AutoMasteringEngine() = default;

AutoMasteringEngine::~AutoMasteringEngine()
{
    stopTimer();
    neuralComp_.stop();
    genreClassifier_.stop();
    eqTranslator_.stop();
}

void AutoMasteringEngine::prepare(double sampleRate, int maxBlockSize)
{
    sampleRate_ = sampleRate;
    blockSize_  = maxBlockSize;

    // Prepare all DSP stages
    splitter_.prepare(sampleRate, maxBlockSize);
    dynamics_.prepare(sampleRate, maxBlockSize);
    eq_.prepare(sampleRate, maxBlockSize);
    stereo_.prepare(sampleRate, maxBlockSize);
    exciter_.prepare(sampleRate, maxBlockSize);
    limiter_.prepare(sampleRate, maxBlockSize);
    lufs_.prepare(sampleRate, maxBlockSize);
    normalizer_.prepare(sampleRate, lufs_);

    // Pre-allocate band buffers
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b)
        bandBuffers_[b].setSize(2, maxBlockSize);

    // Wire intelligence layer
    neuralComp_.prepare(dynamics_, sampleRate);
    neuralComp_.start();

    genreClassifier_.start();

    eqTranslator_.setUpdateCallback([this](int band, const AdaptiveEQ::BandParams& p)
    {
        eq_.setBand(band, p);
    });

    chainPlanner_.setPlanCallback([this](const MultiEffectPlan& plan)
    {
        applyPlan(plan);
    });

    monoChecker_.setCorrectCallback([this](int /*band*/, float sideGainMult)
    {
        // Reduce all stereo widths when mono compat issue detected
        for (int b = 0; b < 4; ++b)
            stereo_.setWidth(b, 1.0f * sideGainMult);
    });

    // Apply genre warm-start defaults immediately
    eqTranslator_.applyHeuristicWarmStart("neutral");

    // Start the 10 Hz orchestration timer
    startTimerHz(10);
}

void AutoMasteringEngine::reset() noexcept
{
    splitter_.reset();
    dynamics_.reset();
    eq_.reset();
    stereo_.reset();
    exciter_.reset();
    limiter_.reset();
    lufs_.reset();
    normalizer_.reset();
    for (auto& b : bandBuffers_)
        b.clear();
}

void AutoMasteringEngine::sumBands(juce::AudioBuffer<float> bands[MultibandSplitter::kNumBands],
                                   juce::AudioBuffer<float>& out) noexcept
{
    out.clear();
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b)
    {
        for (int ch = 0; ch < out.getNumChannels(); ++ch)
        {
            if (ch < bands[b].getNumChannels())
                out.addFrom(ch, 0, bands[b], ch, 0, out.getNumSamples());
        }
    }
}

void AutoMasteringEngine::processBlock(juce::AudioBuffer<float>& buf) noexcept
{
    if (!active_.load(std::memory_order_relaxed)) return;

    const int ns  = buf.getNumSamples();
    const int nch = buf.getNumChannels();
    if (ns == 0 || nch == 0) return;

    // ── Stage 1: M/S encode ────────────────────────────────────────────────
    MSMatrix::encodeBuffer(buf);

    // ── Stage 2: 4-band split ──────────────────────────────────────────────
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b)
    {
        bandBuffers_[b].setSize(nch, ns, false, false, true);
        for (int ch = 0; ch < nch; ++ch)
            bandBuffers_[ch < bandBuffers_[b].getNumChannels() ? ch : 0];
        // Copy M/S buf into each band buffer for splitting
        for (int ch = 0; ch < nch; ++ch)
            bandBuffers_[b].copyFrom(ch, 0, buf, ch, 0, ns);
    }
    splitter_.processBlock(buf, bandBuffers_);

    // ── Stage 3: Per-band dynamics ─────────────────────────────────────────
    dynamics_.processBlock(bandBuffers_);

    // ── Stage 4: Sum bands → M/S buffer ───────────────────────────────────
    sumBands(bandBuffers_, buf);

    // ── Stage 5: Adaptive EQ ──────────────────────────────────────────────
    eq_.processBlock(buf);

    // ── Stage 6: Stereo imager ────────────────────────────────────────────
    stereo_.processBlock(buf);

    // ── Stage 7: Harmonic exciter (optional) ──────────────────────────────
    exciter_.processBlock(buf);

    // ── Stage 8: Brickwall limiter ────────────────────────────────────────
    limiter_.processBlock(buf);

    // ── Stage 9: LUFS metering ────────────────────────────────────────────
    lufs_.processBlock(buf.getArrayOfReadPointers(), buf.getNumChannels(), buf.getNumSamples());

    // ── Stage 10: Loudness normalization ──────────────────────────────────
    normalizer_.processBlock(buf);

    // ── Stage 11: M/S decode ──────────────────────────────────────────────
    MSMatrix::decodeBuffer(buf);

    // ── Mono compatibility check (accumulate; check fires on message thread)
    monoChecker_.accumulateSamples(buf);
}

void AutoMasteringEngine::timerCallback()
{
    ++tickCount_;

    // Every 1 tick (100ms): update loudness normalizer correction gain
    normalizer_.updateCorrectionGain();

    // Every 300 ticks (30s): run chain planner
    if (tickCount_ % plannerUpdateInterval_ == 0)
    {
        const float lra  = lufs_.getLRA();
        const int   genre = genreClassifier_.getTopGenre();

        // Run on this timer callback (message thread) — non-blocking heuristic
        chainPlanner_.executePlan(genre, lra, 0.f, 0.7f);
    }
}

void AutoMasteringEngine::applyPlan(const MultiEffectPlan& plan)
{
    // Apply EQ prescription
    if (plan.eqPrescriptionJSON.isNotEmpty())
        eqTranslator_.applyFromJSON(plan.eqPrescriptionJSON);

    // Apply stereo widths
    for (int b = 0; b < 4; ++b)
        stereo_.setWidth(b, plan.widthCurve[b]);

    // Apply loudness target
    normalizer_.setTargetLUFS(plan.targetLUFS);
    limiter_.setCeiling(plan.ceilingDBTP);

    // Enable/disable exciter
    exciter_.setEnabled(plan.exciterEnabled);
}

} // namespace more_phi
