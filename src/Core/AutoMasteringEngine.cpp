/*
 * More-Phi — Core/AutoMasteringEngine.cpp
 */
#include "AutoMasteringEngine.h"
#include "AI/SonicMasterDecisionDecoder.h"   // AUDIT-2/3: model band counts + dynamics slot layout
#include <algorithm>
#include <cmath>
#include <cstring>

namespace more_phi {

// AUDIT-3: decoder and engine must clamp compressor ratio to the SAME range.
// If one is widened without the other, telemetry lies by up to 3.3x.
// AUDIT-FIX (A6): max tightened 6.0 -> 4.0 (see SonicMasterDecisionDecoder.h).
static_assert(kSonicMasterCompRatioMin == 1.0f,
              "kSonicMasterCompRatioMin mismatch: decoder and engine disagree");
static_assert(kSonicMasterCompRatioMax == 4.0f,
              "kSonicMasterCompRatioMax mismatch: decoder and engine disagree");

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
    // AUDIT-FIX (RT-safe): bandBuffers_ are pre-allocated in prepare() as
    // (MultibandSplitter::kMaxChannels, maxBlockSize). NEVER call setSize() on
    // the audio thread — it heap-allocates if a host changes channel count or
    // block size mid-stream, violating real-time safety. Clamp to the existing
    // capacity instead; the plugin is stereo so nch <= kMaxChannels always holds
    // for a valid layout, and the host guarantees ns <= maxBlockSize.
    {
        const int bandCh  = MultibandSplitter::kMaxChannels;
        const int copyCh  = juce::jmin(nch, bandCh);
        for (int b = 0; b < MultibandSplitter::kNumBands; ++b)
        {
            jassert(bandBuffers_[b].getNumChannels() >= bandCh);
            jassert(bandBuffers_[b].getNumSamples()  >= ns);
            for (int ch = 0; ch < copyCh; ++ch)
                bandBuffers_[b].copyFrom(ch, 0, buf, ch, 0, ns);
        }
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
    // ponytail: MED-20 — inter-stage clip risk if the limiter mask is off (the
    // neural decoder leaves it off by default). The streaming-safe clamp on every
    // apply (applyValidatedPlan :508-512) mitigates by ensuring the limiter
    // ceiling is never above -1.0 dBTP regardless of mask state.
    normalizer_.processBlock(buf);
    // ponytail MED-20: inter-stage clip risk — the normalizer can apply up to
    // +6 dB gain before the limiter. If the neural plan's limiter mask is OFF
    // (the default: limiter is high-risk) and the heuristic also has it off,
    // there is no terminal brickwall, so a loudness target of -14 combined with
    // high makeup gain could clip the DAC. The streaming-safe ceiling clamp in
    // applyValidatedPlan guards the apply path; the heuristic applyPlan() does
    // the same via kStreamingSafeCeilingDBTP. This is still less protection than
    // having the limiter on — the clamp only caps the ceiling, not the gain
    // before it.

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
    sample.thdPercent = hasSpectrum ? spectrum.thdPercent : 0.0f;
    sample.crestFactorProgram = hasSpectrum ? spectrum.crestFactorProgram : 0.0f;
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
    // AUDIT CRITICAL-7: the bridge re-enters this function via chainPlanner_'s
    // callback. Without these guards the heuristic unconditionally overwrites
    // the limiter ceiling and exciter enable the neural path just set. Defer to
    // the neural plan when it holds those masks.
    const bool neuralHasLimiter  = hasNeuralPlan && lastSafeNeuralPlan_.appliedMask.limiter;
    const bool neuralHasHarmonic = hasNeuralPlan && lastSafeNeuralPlan_.appliedMask.harmonic;

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
    if (!neuralHasLimiter)
        limiter_.setCeiling(std::min(plan.ceilingDBTP, kStreamingSafeCeilingDBTP));

    // Enable/disable exciter
    if (!neuralHasHarmonic)
        exciter_.setEnabled(plan.exciterEnabled);
}

bool AutoMasteringEngine::applyValidatedPlan(const ValidatedNeuralMasteringPlan& plan) noexcept
{
    // ponytail: MED-10 — instant parameter set is fine because the internal chain
    // is dormant in the shipped plugin. If activated, add 5-10ms ramps to avoid
    // discontinuities on live audio.
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
    else if (plan.applyLimiterCeiling)
    {
        // Limiter ceiling / AUDIT: opt-in path. The decoder leaves
        // appliedMask.limiter OFF by default (true-peak limiting is high-risk);
        // plan.applyLimiterCeiling lets a caller explicitly request the decoded
        // ceiling be honoured, hard-clamped to the streaming-safe ceiling so the
        // decision can never produce an inter-sample clip regardless of the
        // model's emitted value.
        const auto requested = std::clamp(-1.0f + plan.projectedTargets.limiter[0] * 0.5f,
                                          -3.0f, -0.1f);
        limiter_.setCeiling(std::min(requested, kStreamingSafeCeilingDBTP));
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

    // AUDIT CRITICAL-6/7/17: bridge the neural plan to the hosted mastering
    // plugin. The internal DSP chain above is dormant in the shipped plugin
    // (prepare(...,false)); the neural model's decisions only reach audio if we
    // forward them as a MultiEffectPlan through the OzonePlanApplicator path.
    // chainPlanner_.applyPlan re-enters the heuristic applyPlan() above via its
    // callback_, but that path now defers to lastSafeNeuralPlan_ on every
    // contested stage (EQ/stereo/loudness/limiter/harmonic), so the re-entrant
    // call is a no-op for the stages this function just set. The return value is
    // the count of hosted-plugin parameters enqueued; 0 when no applicator is
    // registered or the map is all-stubs.
    //
    // ponytail MED-10: instant-apply is safe while the internal chain is
    // dormant. If the chain is ever activated as a realtime audio processor,
    // the internal DSP parameter writes above (eq_.setBandGain, limiter_.setCeiling,
    // etc.) should be ramped over 5-10ms to avoid clicks. The bridge path
    // (chainPlanner_.applyPlan) already ramps through ParameterBridge's
    // smoothing layer.
    MultiEffectPlan multiPlan = buildBridgePlanFromNeural(plan);
    const int ozoneApplied = chainPlanner_.applyPlan(multiPlan);
    lastOzoneAppliedCount_.store(ozoneApplied, std::memory_order_release);

    // P0.2: if an Ozone applicator IS registered but applied 0 params, the map
    // is all-stubs (audit never ran). Returning true with all-stubs misleads
    // callers into thinking the neural plan reached audio — it didn't.
    if (ozoneApplied == 0 && chainPlanner_.hasOzoneApplicator())
        return false;

    lastSafeNeuralPlan_ = plan;
    hasLastSafeNeuralPlan_ = true;
    return true;
}

void AutoMasteringEngine::clearLastSafeNeuralMasteringPlan() noexcept
{
    lastSafeNeuralPlan_ = {};
    hasLastSafeNeuralPlan_ = false;
}

MultiEffectPlan AutoMasteringEngine::buildBridgePlanFromNeural(
    const ValidatedNeuralMasteringPlan& plan) const noexcept
{
    MultiEffectPlan p;
    p.valid = true;
    p.useNeuralComp = true;

    // EQ: 8 model bands @ 60/120/250/500/1k/2.5k/5k/10k Hz, gain = eq[i]*12 dB,
    // Q 0.707, type "peak". Matches the schema OzonePlanApplicator::applyEQ
    // consumes (freq/gain/Q/type per band).
    {
        juce::String json = "{ \"bands\": [";
        for (std::size_t i = 0; i < kSonicMasterEqGainCount; ++i)
        {
            const float gainDb = std::clamp(plan.projectedTargets.eq[i]
                                                * AdaptiveEQ::kMaxGainDB,
                                            -AdaptiveEQ::kMaxGainDB,
                                            AdaptiveEQ::kMaxGainDB);
            if (i > 0) json += ", ";
            // ponytail: fixed Q/type; the neural plan carries no per-band Q/type.
            json += "{ \"freq\": " + juce::String(kSonicMasterEqFrequenciesHz[i], 0)
                  + ", \"gain\": " + juce::String(gainDb, 2)
                  + ", \"Q\": 0.707, \"type\": \"peak\" }";
        }
        json += "] }";
        p.eqPrescriptionJSON = json;
    }

    // Compression need [0..1]: derive from the sidecar ratios when present,
    // else from the normalized dynamics pair. Higher ratio → more need.
    if (plan.hasCompParams)
    {
        float ratioSum = 0.0f;
        for (std::size_t b = 0; b < kNeuralMasteringCompBandCount; ++b)
            ratioSum += plan.compParams[b].ratio;
        const float avgRatio = ratioSum / static_cast<float>(kNeuralMasteringCompBandCount);
        // Map ratio [1..6] → [0..1].
        p.compressionNeed = std::clamp((avgRatio - 1.0f) / 5.0f, 0.0f, 1.0f);
    }
    else
    {
        // Dynamics[2b+1] is the normalized ratio in [~0.4..~3.5]; map onto [0..1].
        float val = 0.0f;
        int n = 0;
        for (std::size_t b = 0; b < kSonicMasterCompBandCount; ++b)
        {
            val += plan.projectedTargets.dynamics[b * kSonicMasterDynamicsSlotsPerBand + 1];
            ++n;
        }
        p.compressionNeed = std::clamp(val / static_cast<float>(n) * 0.5f + 0.2f, 0.0f, 1.0f);
    }

    // Stereo width: the neural plan fills 2 regions; copy into widthCurve[0..1]
    // and leave [2..3] at neutral so the hosted imager's mid/high bands aren't
    // disturbed by the bridge.
    p.widthCurve[0] = 0.0f;
    p.widthCurve[1] = 0.6f;
    p.widthCurve[2] = 1.0f;
    p.widthCurve[3] = 1.4f;
    if (plan.appliedMask.stereo)
    {
        for (std::size_t r = 0; r < kSonicMasterStereoRegionCount && r < 4; ++r)
            // width = clamp(1.0 + v, 0, 2), matching applyValidatedPlan's mapping.
            p.widthCurve[r] = std::clamp(1.0f + plan.projectedTargets.stereo[r], 0.0f, 2.0f);
    }

    // Loudness target (LUFS) and ceiling (dBTP): same inverse of the decoder
    // math applyValidatedPlan uses, so the hosted plugin sees the same target the
    // internal normalizer/limiter would have enforced.
    p.targetLUFS  = std::clamp(-14.0f + plan.projectedTargets.loudness[0] * 6.0f, -23.0f, -8.0f);
    const float rawCeiling = std::clamp(-1.0f + plan.projectedTargets.limiter[0] * 0.5f, -3.0f, -0.1f);
    p.ceilingDBTP = std::min(rawCeiling, kStreamingSafeCeilingDBTP);

    p.exciterEnabled = plan.appliedMask.harmonic
                       ? (plan.projectedTargets.harmonic[0] > 0.01f)
                       : false;

    return p;
}

// AUDIT MED-11: persist the last applied neural plan across save/restore.
// ValidatedNeuralMasteringPlan is an aggregate of scalars + fixed-size arrays
// (no pointers, no ownership), so a base64 blit is safe and round-trippable.
void AutoMasteringEngine::serializeLastPlan(juce::XmlElement& parent) const
{
    if (!hasLastSafeNeuralPlan_)
        return;

    auto* el = parent.createNewChildElement("MASTERING_PLAN");
    el->setAttribute("version", static_cast<int>(kNeuralMasteringPlanSchemaVersion));
    el->setAttribute("hasPlan", true);

    // Blit the POD plan into a base64 string.
    const auto* raw = reinterpret_cast<const char*>(&lastSafeNeuralPlan_);
    const auto size = static_cast<int>(sizeof(ValidatedNeuralMasteringPlan));
    const juce::String base64 = juce::Base64::toBase64(raw, size);
    el->setAttribute("data", base64);
}

bool AutoMasteringEngine::restoreLastPlan(const juce::XmlElement& parent)
{
    const auto* el = parent.getChildByName("MASTERING_PLAN");
    if (el == nullptr)
        return false;

    const juce::String base64 = el->getStringAttribute("data");
    if (base64.isEmpty())
        return false;

    ValidatedNeuralMasteringPlan restored {};
    juce::MemoryOutputStream mos;
    if (!juce::Base64::convertFromBase64(mos, base64))
        return false;

    const auto decodedSize = mos.getDataSize();
    if (decodedSize != sizeof(ValidatedNeuralMasteringPlan))
        return false;

    std::memcpy(&restored, mos.getData(), sizeof(restored));
    if (!restored.valid)
        return false;

    lastSafeNeuralPlan_ = restored;
    hasLastSafeNeuralPlan_ = true;
    return true;
}

} // namespace more_phi
