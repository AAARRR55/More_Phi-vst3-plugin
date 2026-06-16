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

void AutoMasteringEngine::prepare(double sampleRate, int maxBlockSize, bool startIntelligence)
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
    analysisTruePeak_.prepare(sampleRate, maxBlockSize);
    lufs_.prepare(sampleRate, maxBlockSize);
    normalizer_.prepare(sampleRate, lufs_);
    spectrumAnalyzer_.prepare(sampleRate, maxBlockSize);
    stereoFieldAnalyzer_.prepare(sampleRate, maxBlockSize);
    meterWindow_.reset();
    analysisElapsedSeconds_ = 0.0;
    analysisSamplesSinceWindowSample_ = 0;
    analysisSumSquares_ = 0.0;
    analysisSampleCount_ = 0;

    // Pre-allocate band buffers
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b)
        bandBuffers_[b].setSize(2, maxBlockSize);

    // Wire intelligence layer
    if (startIntelligence)
    {
        neuralComp_.prepare(dynamics_, sampleRate);
        neuralComp_.start();
        genreClassifier_.start();
    }

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

    // Start the 10 Hz orchestration timer only for the autonomous mastering
    // engine. Hosted-plugin metering uses analyzeBlock() and must not start
    // background plan updates.
    if (startIntelligence)
        startTimerHz(10);
    else
        stopTimer();
}

void AutoMasteringEngine::reset() noexcept
{
    splitter_.reset();
    dynamics_.reset();
    eq_.reset();
    stereo_.reset();
    exciter_.reset();
    limiter_.reset();
    analysisTruePeak_.reset();
    lufs_.reset();
    normalizer_.reset();
    meterWindow_.reset();
    spectrumAnalyzer_.reset();
    stereoFieldAnalyzer_.reset();
    smoothedSpectralTilt_ = 0.0f;
    analysisElapsedSeconds_ = 0.0;
    analysisSamplesSinceWindowSample_ = 0;
    analysisSumSquares_ = 0.0;
    analysisSampleCount_ = 0;
    clearLastSafeNeuralMasteringPlan();
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

    // ── Stage 8: Loudness normalization ──────────────────────────────────
    // LUFS-1 FIX: normalize BEFORE the limiter. The normalizer applies up to
    // +6 dB correction gain; running it AFTER the limiter (the old order) pushed
    // the already-limited signal back above the dBTP ceiling, defeating the
    // limiter and the B-1 true-peak fix. Now the brickwall limiter is the
    // terminal gain stage and catches any overshoot the normalizer introduces.
    normalizer_.processBlock(buf);

    // ── Stage 9: Brickwall limiter (terminal gain stage) ─────────────────
    limiter_.processBlock(buf);

    // ── Stage 10: Meter the FINAL delivered output ───────────────────────
    // Both meters now read the post-normalization + post-limit signal, so the
    // reported dBTP/LUFS match what is actually delivered, and the normalizer's
    // feedback loop (it reads meter_->getIntegrated()) converges on the target.
    analysisTruePeak_.processBlock(buf);
    lufs_.processBlock(buf.getArrayOfReadPointers(), buf.getNumChannels(), buf.getNumSamples());

    // ── Stage 11: M/S decode ──────────────────────────────────────────────
    MSMatrix::decodeBuffer(buf);

    spectrumAnalyzer_.processBlock(buf);
    stereoFieldAnalyzer_.processBlock(buf);
    updateAnalysisWindow(buf);

    // ── Mono compatibility check (accumulate; check fires on message thread)
    monoChecker_.accumulateSamples(buf);
}

int AutoMasteringEngine::getMasteringChainLatency() const noexcept
{
    // ENHANCERS-1/PDC: report 0 while the chain is dormant (the shipped plugin
    // only meters via analyzeBlock), so the live plugin's reported latency is
    // unchanged. When mastering is engaged, report the lookahead stages: the
    // brickwall limiter (always in the chain) plus the exciter's 4x oversampling
    // delay when it is enabled. Both were previously never reported (the
    // masteringChainLatency slot in LatencyManager was dead).
    if (!active_.load(std::memory_order_relaxed)) return 0;
    int latency = limiter_.getLookaheadSamples();
    latency += exciter_.getLatencyInSamples();  // 0 when the exciter is disabled
    return latency;
}

void AutoMasteringEngine::analyzeBlock(const juce::AudioBuffer<float>& buf) noexcept
{
    const int ns  = buf.getNumSamples();
    const int nch = buf.getNumChannels();
    if (ns == 0 || nch == 0)
        return;

    lufs_.processBlock(buf.getArrayOfReadPointers(), nch, ns);
    analysisTruePeak_.processBlock(buf);
    spectrumAnalyzer_.processBlock(buf);
    stereoFieldAnalyzer_.processBlock(buf);
    updateAnalysisWindow(buf);
}

void AutoMasteringEngine::updateAnalysisWindow(const juce::AudioBuffer<float>& buf) noexcept
{
    const int ns = buf.getNumSamples();
    const int nch = buf.getNumChannels();
    if (ns <= 0 || nch <= 0 || sampleRate_ <= 0.0)
        return;

    for (int ch = 0; ch < nch; ++ch)
    {
        const float* data = buf.getReadPointer(ch);
        for (int i = 0; i < ns; ++i)
        {
            const double v = static_cast<double>(data[i]);
            analysisSumSquares_ += v * v;
            ++analysisSampleCount_;
        }
    }

    analysisElapsedSeconds_ += static_cast<double>(ns) / sampleRate_;
    analysisSamplesSinceWindowSample_ += ns;

    const int sampleInterval = std::max(1, static_cast<int>(sampleRate_ * 0.1));
    if (analysisSamplesSinceWindowSample_ < sampleInterval)
        return;

    const int samplesToEmit = analysisSamplesSinceWindowSample_ / sampleInterval;
    analysisSamplesSinceWindowSample_ %= sampleInterval;

    RealtimeSpectrumAnalyzer::SpectrumSnapshot spectrum;
    const bool hasSpectrum = spectrumAnalyzer_.getSnapshot(spectrum);

    StereoFieldAnalyzer::StereoFieldSnapshot stereo;
    const bool hasStereo = stereoFieldAnalyzer_.getSnapshot(stereo);

    MeterWindowAccumulator::MeterSample sample;
    sample.timestampSeconds = analysisElapsedSeconds_;
    sample.rms = analysisSampleCount_ > 0
        ? static_cast<float>(std::sqrt(analysisSumSquares_ / static_cast<double>(analysisSampleCount_)))
        : 0.0f;
    sample.lufsMomentary = getLUFSMomentary();
    sample.lufsShortTerm = getLUFSShortTerm();
    sample.lufsIntegrated = getLUFSIntegrated();
    sample.lra = getLRA();
    sample.truePeakDBTP = getTruePeak_dBTP();
    sample.limiterGRDB = getLimiterGainReductionDB();
    sample.spectralCentroidHz = hasSpectrum ? spectrum.spectralCentroid : 0.0f;
    sample.spectralTiltDBPerOctave = hasSpectrum ? spectrum.spectralTilt : 0.0f;
    sample.stereoWidth = hasStereo ? stereo.stereoWidth : 0.0f;
    sample.midBandCorrelation = hasStereo ? stereo.correlation[2] : 0.0f;

    // Reset accumulators for next interval
    analysisSumSquares_ = 0.0;
    analysisSampleCount_ = 0;

    for (int emitted = 0; emitted < samplesToEmit; ++emitted)
    {
        const int intervalsFromEnd = samplesToEmit - emitted - 1;
        sample.timestampSeconds = analysisElapsedSeconds_
            - static_cast<double>(analysisSamplesSinceWindowSample_
                + intervalsFromEnd * sampleInterval) / sampleRate_;
        meterWindow_.pushSample(sample);
    }
}

void AutoMasteringEngine::timerCallback()
{
    ++tickCount_;

    // Every 1 tick (100ms): update loudness normalizer correction gain
    normalizer_.updateCorrectionGain();

    RealtimeSpectrumAnalyzer::SpectrumSnapshot spectrumSnapshot;
    if (spectrumAnalyzer_.getSnapshot(spectrumSnapshot) && spectrumSnapshot.frameIndex > 0)
    {
        constexpr float smoothing = 0.1f;
        smoothedSpectralTilt_ += smoothing * (spectrumSnapshot.spectralTilt - smoothedSpectralTilt_);
    }

    float correlationMS = 0.7f;
    StereoFieldAnalyzer::StereoFieldSnapshot stereoSnapshot;
    if (stereoFieldAnalyzer_.getSnapshot(stereoSnapshot) && stereoSnapshot.frameIndex > 0)
    {
        correlationMS = stereoSnapshot.correlation[2] < 0.0f
            ? stereoSnapshot.correlation[2]
            : std::clamp(1.0f - stereoSnapshot.stereoWidth, 0.0f, 1.0f);
    }

    // Every 300 ticks (30s): run chain planner
    if (tickCount_ % plannerUpdateInterval_ == 0)
    {
        const float lra  = lufs_.getLRA();
        const int   genre = genreClassifier_.getTopGenre();

        // Run on this timer callback (message thread) — non-blocking heuristic
        chainPlanner_.executePlan(genre, lra, smoothedSpectralTilt_, correlationMS);
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

bool AutoMasteringEngine::applyValidatedPlan(const ValidatedNeuralMasteringPlan& plan) noexcept
{
    if (!plan.valid || plan.fallbackMode != NeuralMasteringFallbackMode::None)
        return false;

    if (plan.appliedMask.eq)
    {
        for (int band = 0; band < AdaptiveEQ::kNumBands; ++band)
            eq_.setBandGain(band, std::clamp(plan.projectedTargets.eq[static_cast<std::size_t>(band)]
                                             * AdaptiveEQ::kMaxGainDB,
                                             -AdaptiveEQ::kMaxGainDB,
                                             AdaptiveEQ::kMaxGainDB));
    }

    if (plan.appliedMask.dynamics)
    {
        for (int band = 0; band < MultibandDynamicsProcessor::kNumBands; ++band)
        {
            auto params = dynamics_.getBandParams(band);
            const auto value = plan.projectedTargets.dynamics[static_cast<std::size_t>(band)];
            params.thresholdDB = std::clamp(-20.0f + value * 8.0f, -40.0f, -6.0f);
            params.ratio = std::clamp(2.5f + value * 1.5f, 1.0f, 6.0f);
            dynamics_.setBandParams(band, params);
        }
    }

    if (plan.appliedMask.stereo)
    {
        for (int region = 0; region < StereoImager::kNumRegions; ++region)
        {
            const auto value = plan.projectedTargets.stereo[static_cast<std::size_t>(region)];
            stereo_.setWidth(region, std::clamp(1.0f + value, 0.0f, 2.0f));
        }
    }

    if (plan.appliedMask.harmonic)
    {
        const auto amount = std::clamp(plan.projectedTargets.harmonic[0], 0.0f, 1.0f);
        exciter_.setEnabled(amount > 0.01f);
        exciter_.setDrive(std::clamp(6.0f + amount * 12.0f, 0.0f, 18.0f));
        exciter_.setDryWet(std::clamp(amount, 0.0f, 0.6f));
    }

    if (plan.appliedMask.limiter)
    {
        const auto ceiling = std::clamp(-1.0f + plan.projectedTargets.limiter[0] * 0.5f,
                                        -3.0f,
                                        -0.1f);
        limiter_.setCeiling(ceiling);
    }

    if (plan.appliedMask.loudness)
    {
        const auto target = std::clamp(-14.0f + plan.projectedTargets.loudness[0] * 6.0f,
                                       -23.0f,
                                       -8.0f);
        normalizer_.setTargetLUFS(target);
    }

    lastSafeNeuralPlan_ = plan;
    hasLastSafeNeuralPlan_ = true;
    return true;
}

void AutoMasteringEngine::clearLastSafeNeuralMasteringPlan() noexcept
{
    lastSafeNeuralPlan_ = {};
    hasLastSafeNeuralPlan_ = false;
}

} // namespace more_phi
