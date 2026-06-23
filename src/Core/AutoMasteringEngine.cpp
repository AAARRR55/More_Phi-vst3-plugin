/*
 * More-Phi — Core/AutoMasteringEngine.cpp
 */
#include "AutoMasteringEngine.h"
#include "AI/SonicMasterDecisionDecoder.h"   // AUDIT-2/3: model band counts + dynamics slot layout
#include <algorithm>
#include <cmath>

namespace more_phi {

namespace {
// ponytail: inlined from the removed NeuralCompressor — its loadModel() always
// returned false and its 33Hz timer was a no-op, so the only real effect was
// applying these published-literature per-band compressor defaults once.
const MultibandDynamicsProcessor::BandParams kHeuristicDefaults[] = {
    { .thresholdDB = -18.f, .ratio = 1.5f, .attackMs =  50.f, .releaseMs = 200.f, .makeupDB = 0.f, .kneeDB = 4.f }, // Band 0: Sub
    { .thresholdDB = -20.f, .ratio = 2.5f, .attackMs =  15.f, .releaseMs = 150.f, .makeupDB = 0.f, .kneeDB = 3.f }, // Band 1: Low
    { .thresholdDB = -22.f, .ratio = 3.0f, .attackMs =   8.f, .releaseMs = 120.f, .makeupDB = 0.f, .kneeDB = 2.f }, // Band 2: Mid
    { .thresholdDB = -18.f, .ratio = 2.0f, .attackMs =   3.f, .releaseMs =  80.f, .makeupDB = 0.f, .kneeDB = 2.f }, // Band 3: High
};

// AUDIT-FIX-2: hard upper bound on the limiter ceiling enforced after every
// neural/heuristic apply. -1.0 dBTP is the streaming-platform standard
// (Spotify, YouTube, Apple Music). Lower ceilings are always allowed.
constexpr float kStreamingSafeCeilingDBTP = -1.0f;
} // namespace

AutoMasteringEngine::AutoMasteringEngine() = default;

AutoMasteringEngine::~AutoMasteringEngine()
{
    stopTimer();
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
        // ponytail: NeuralCompressor was a 33Hz no-op timer around this
        // heuristic table (loadModel() always returned false). Apply the
        // published-literature per-band defaults directly to the dynamics stage.
        for (int b = 0; b < MultibandSplitter::kNumBands; ++b)
            dynamics_.setBandParams(b, kHeuristicDefaults[b]);
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
    // AUDIT-FIX (M1): The previous loop body contained a dead expression
    // statement `bandBuffers_[ch < ... ? ch : 0];` that indexed the array and
    // discarded the result — a leftover from a refactor that produced no code.
    // Removed: setSize + copyFrom already establish per-band channel counts and
    // contents, and the splitter overwrites the band data immediately after.
    for (int b = 0; b < MultibandSplitter::kNumBands; ++b)
    {
        bandBuffers_[b].setSize(nch, ns, false, false, true);
        jassert(bandBuffers_[b].getNumChannels() >= nch); // setSize must give us nch channels
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

    // ── Stage 9: M/S decode ──────────────────────────────────────────────
    // MSDECODE-1 FIX: decode to L/R BEFORE the brickwall limiter and the meter.
    // Previously decode ran AFTER both (stage 11), which meant the limiter
    // enforced the dBTP ceiling on the M/S representation while the meter also
    // read M/S — but M/S decode is L = mid + side (MSMatrix.h:36, no /sqrt2), so
    // two channels each near the ceiling sum to ~+6 dBFS after decode. The
    // limiter never saw it and the meter under-read by several dB, so the model
    // eval reported a safe -0.91 dBTP while the delivered output clipped at
    // +3.45 dBFS. Decoding here puts the limiter on the actual delivered L/R so
    // the ceiling holds, and the meter below reads the same signal that ships.
    MSMatrix::decodeBuffer(buf);

    // ── Stage 10: Brickwall limiter (terminal gain stage) ────────────────
    limiter_.processBlock(buf);

    // ── Stage 11: Meter the FINAL delivered output ───────────────────────
    // Both meters read the post-decode + post-limit L/R signal, so the reported
    // dBTP/LUFS match what is actually delivered, and the normalizer's feedback
    // loop (it reads meter_->getIntegrated()) converges on the target.
    analysisTruePeak_.processBlock(buf);
    lufs_.processBlock(buf.getArrayOfReadPointers(), buf.getNumChannels(), buf.getNumSamples());

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
    // ponytail: feed the genre classifier here (the only place we have a full
    // audio buffer on a non-RT message-thread path). Without this the classifier
    // starves and the whole AI decision chain is stuck on the default genre.
    genreClassifier_.feedAudio(buf, sampleRate_);
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
    // AUDIT-FIX-9: reconcile dual writers. The 30s heuristic timer used to call
    // setTargetLUFS / setWidth unconditionally, clobbering whatever the neural
    // path (applyValidatedPlan) had just set. Now the heuristic defers loudness,
    // stereo width, AND EQ (AUDIT-FIX-R6) to a recent neural plan when one
    // exists — those are the three knobs both paths fight over.
    // EQ/ceiling/exciter still come from the heuristic because the neural path
    // only decides EQ bands 0-7 and (optionally) the ceiling; band 8-31 EQ and
    // exciter enable are heuristic-authoritative when the neural plan doesn't
    // touch EQ.
    const bool hasNeuralPlan = hasLastSafeNeuralPlan_;
    const bool neuralHasEq     = hasNeuralPlan && lastSafeNeuralPlan_.appliedMask.eq;
    const bool neuralHasLoud   = hasNeuralPlan && lastSafeNeuralPlan_.appliedMask.loudness;
    const bool neuralHasStereo = hasNeuralPlan && lastSafeNeuralPlan_.appliedMask.stereo;

    // Apply EQ prescription — defer to neural if it controls EQ bands 0-7.
    // AUDIT-FIX-R6: previously the heuristic always applied its full 32-band
    // genre curve, overwriting the neural EQ bands 0-7 after the neural path
    // had just set them. Now we skip the heuristic EQ when the neural plan
    // holds the EQ mask. Bands 8-31 retain whatever the last heuristic or
    // genre-translator pass set them to.
    if (!neuralHasEq && plan.eqPrescriptionJSON.isNotEmpty())
        eqTranslator_.applyFromJSON(plan.eqPrescriptionJSON);

    // Apply stereo widths — only when the neural path is NOT controlling stereo.
    if (!neuralHasStereo)
        for (int b = 0; b < 4; ++b)
            stereo_.setWidth(b, plan.widthCurve[b]);

    // Apply loudness target — only when the neural path is NOT controlling it.
    if (!neuralHasLoud)
        normalizer_.setTargetLUFS(plan.targetLUFS);

    // AUDIT-FIX-2 / AUDIT-FIX-9: cap the heuristic ceiling at streaming-safe.
    // The plan's ceilingDBTP may be looser than -1.0 dBTP; clamp it down so the
    // 30s heuristic timer cannot relax the ceiling that the neural path (or a
    // prior call) tightened. Tighter ceilings always win.
    limiter_.setCeiling(std::min(plan.ceilingDBTP, kStreamingSafeCeilingDBTP));

    // Enable/disable exciter
    exciter_.setEnabled(plan.exciterEnabled);
}

bool AutoMasteringEngine::applyValidatedPlan(const ValidatedNeuralMasteringPlan& plan) noexcept
{
    if (!plan.valid || plan.fallbackMode != NeuralMasteringFallbackMode::None)
        return false;

    if (plan.appliedMask.eq)
    {
        // AUDIT-2: apply ONLY the EQ bands the SonicMaster model decides on
        // (kSonicMasterEqGainCount = 8). The AdaptiveEQ exposes 32 bands; the
        // other 24 stay on the genre translator's warm-start. Previously the
        // loop ran to kNumBands and force-wrote bands 8..31 to 0 dB, wiping the
        // genre character (warmth/presence) the translator had set.
        for (int band = 0; band < static_cast<int>(kSonicMasterEqGainCount); ++band)
            eq_.setBandGain(band, std::clamp(plan.projectedTargets.eq[static_cast<std::size_t>(band)]
                                             * AdaptiveEQ::kMaxGainDB,
                                             -AdaptiveEQ::kMaxGainDB,
                                             AdaptiveEQ::kMaxGainDB));
    }

    if (plan.appliedMask.dynamics)
    {
        // AUDIT-2/3: apply only the 3 model bands; band 3 (High) stays on the
        // heuristic warm-start. AUDIT-2.1: when the plan carries the full
        // real-unit compParams sidecar (SonicMaster decisions do), apply all six
        // params per band directly. Otherwise fall back to the threshold/ratio
        // pair decoded into the normalized dynamics array and leave attack/
        // release/makeup/knee on whatever the band already had.
        const int bandCount = static_cast<int>(plan.hasCompParams
            ? kNeuralMasteringCompBandCount
            : kSonicMasterCompBandCount);
        for (int band = 0; band < bandCount; ++band)
        {
            auto params = dynamics_.getBandParams(band);
            if (plan.hasCompParams)
            {
                const auto& cp = plan.compParams[static_cast<std::size_t>(band)];
                params.thresholdDB = std::clamp(cp.thresholdDb, -40.0f, -6.0f);
                params.ratio       = std::clamp(cp.ratio,        kSonicMasterCompRatioMin, kSonicMasterCompRatioMax);
                params.attackMs    = std::clamp(cp.attackMs,     0.1f, 100.0f);
                params.releaseMs   = std::clamp(cp.releaseMs,   10.0f, 500.0f);
                params.makeupDB    = std::clamp(cp.makeupDb,     0.0f,  12.0f);
                params.kneeDB      = std::clamp(cp.kneeDb,       0.0f,  12.0f);
            }
            else
            {
                const std::size_t base = static_cast<std::size_t>(band) * kSonicMasterDynamicsSlotsPerBand;
                const auto thrValue = plan.projectedTargets.dynamics[base + 0];
                const auto ratValue = plan.projectedTargets.dynamics[base + 1];
                params.thresholdDB = std::clamp(-20.0f + thrValue * 8.0f, -40.0f, -6.0f);
                params.ratio = std::clamp(2.5f + ratValue * 1.5f, kSonicMasterCompRatioMin, kSonicMasterCompRatioMax);
            }
            dynamics_.setBandParams(band, params);
        }
    }

    if (plan.appliedMask.stereo)
    {
        // AUDIT-2: apply only the 2 model width regions; the other 2 regions
        // (Mid/High) stay on the genre translator / mono-checker callback.
        for (int region = 0; region < static_cast<int>(kSonicMasterStereoRegionCount); ++region)
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

    // AUDIT-FIX-2: guarantee a streaming-safe terminal ceiling on EVERY neural
    // apply, not just when the limiter mask is on. The SonicMaster decoder leaves
    // appliedMask.limiter OFF (limiter is high-risk), so without this guard a
    // -8 LUFS target combined with 3 bands x +12 dB makeup could run hot into a
    // limiter holding a lax prior ceiling. -1.0 dBTP is the streaming standard
    // (Spotify/YouTube/Apple). The explicit mask path above can still drive a
    // tighter ceiling if the caller opts in; this clamp only caps the upper bound.
    {
        const float currentCeiling = limiter_.getCeiling();
        if (currentCeiling > kStreamingSafeCeilingDBTP)
            limiter_.setCeiling(kStreamingSafeCeilingDBTP);
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
