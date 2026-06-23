/*
 * More-Phi — AI/SonicMasterAnalysisEngine.cpp
 *
 * Implements the realtime neural mastering orchestration loop. See header for
 * the thread model and lifecycle invariants.
 */
#include "AI/SonicMasterAnalysisEngine.h"

#include "Core/AutoMasteringEngine.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

namespace more_phi {

namespace {

float peakAbs(const float* a, std::size_t n) noexcept
{
    float m = 0.0f;
    for (std::size_t i = 0; i < n; ++i)
        m = std::max(m, std::abs(a[i]));
    return m;
}

// Linear resample (matches sonicmaster_engine.api.preprocess_audio so the
// model sees input consistent with how it was validated offline).
// ponytail: linear interpolation resampling — introduces mirror-image aliasing
// above ~0.4 * min(Nyquist_src, Nyquist_dst). At 96 kHz hosts -> 44.1 kHz this
// contaminates content above ~9 kHz. Intentional and in-distribution: the
// model was trained on the same aliased preprocessing, so analysis is
// self-consistent. A polyphase FIR would de-alias but change the model's input
// distribution. Upgrade path: re-preprocess + retrain, not a runtime swap.
void resampleLinear(const float* src, std::size_t srcLen, std::size_t dstLen, float* dst) noexcept
{
    if (srcLen == 0 || dstLen == 0) return;
    if (srcLen == 1) { std::fill_n(dst, dstLen, src[0]); return; }
    for (std::size_t i = 0; i < dstLen; ++i)
    {
        const double pos = static_cast<double>(i) * static_cast<double>(srcLen - 1)
                         / static_cast<double>(dstLen - 1);
        const std::size_t i0 = static_cast<std::size_t>(pos);
        const std::size_t i1 = std::min(i0 + 1, srcLen - 1);
        const double frac = pos - static_cast<double>(i0);
        dst[i] = static_cast<float>(src[i0] * (1.0 - frac) + src[i1] * frac);
    }
}

template <std::size_t N>
void writeClampedDelta(std::array<float, N>& delta,
                       const std::array<float, N>& target,
                       const std::array<float, N>& previous) noexcept
{
    for (std::size_t i = 0; i < N; ++i)
        delta[i] = std::clamp(target[i] - previous[i], -1.0f, 1.0f);
}

MasteringTargetVector makeDeltasTowardTargets(const MasteringTargetVector& targets,
                                              const NeuralMasteringSafetyPolicy& policy) noexcept
{
    MasteringTargetVector previous {};
    if (policy.hasLastSafePlan())
        previous = policy.getLastSafePlan().projectedTargets;

    MasteringTargetVector deltas {};
    writeClampedDelta(deltas.eq,       targets.eq,       previous.eq);
    writeClampedDelta(deltas.dynamics, targets.dynamics, previous.dynamics);
    writeClampedDelta(deltas.stereo,   targets.stereo,   previous.stereo);
    writeClampedDelta(deltas.harmonic, targets.harmonic, previous.harmonic);
    writeClampedDelta(deltas.limiter,  targets.limiter,  previous.limiter);
    writeClampedDelta(deltas.loudness, targets.loudness, previous.loudness);
    return deltas;
}

NeuralMasteringPlanCandidate makeSafetyCandidate(const ValidatedNeuralMasteringPlan& plan,
                                                 NeuralMasteringRuntimeMode mode,
                                                 float confidence,
                                                 const NeuralMasteringSafetyPolicy& policy) noexcept
{
    NeuralMasteringPlanCandidate candidate {};
    candidate.schemaVersion   = kNeuralMasteringPlanSchemaVersion;
    candidate.planId          = plan.sourcePlanId;
    candidate.runtimeMode     = mode;
    candidate.confidence      = confidence;
    candidate.evidenceLevel   = plan.evidenceLevel;
    candidate.editableMask    = plan.appliedMask;
    candidate.targets         = plan.projectedTargets;
    candidate.deltas          = makeDeltasTowardTargets(plan.projectedTargets, policy);
    // AUDIT-FIX: carry compressor sidecar through the safety policy so the
    // verdict preserves the model's full per-band params (attack/release/
    // makeup/knee), not just the normalized threshold/ratio pair.
    candidate.compParams      = plan.compParams;
    candidate.hasCompParams   = plan.hasCompParams;
    candidate.capturedAtSteadyClockNs = plan.capturedAtSteadyClockNs;
    return candidate;
}

} // namespace

SonicMasterAnalysisEngine::SonicMasterAnalysisEngine() = default;

SonicMasterAnalysisEngine::~SonicMasterAnalysisEngine()
{
    release();
}

void SonicMasterAnalysisEngine::setInferenceSource(ISonicMasterInferenceSource* source) noexcept
{
    source_ = source;
}

bool SonicMasterAnalysisEngine::isAvailable() const noexcept
{
    return source_ != nullptr && source_->isAvailable();
}

void SonicMasterAnalysisEngine::ensureRing() noexcept
{
    // Idempotent — only allocates on first call when ring is nullptr.
    if (ring_ != nullptr) return;
    if (!prepared_.load(std::memory_order_relaxed)) return;
    ring_ = std::make_unique<AudioCaptureRing>(config_.captureRingFrames);
}

void SonicMasterAnalysisEngine::prepare(double sampleRate, int /*maxBlockSize*/)
{
    // Re-prepare is allowed: tear down any prior thread first.
    release();

    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    // PERF-MEM: Defer AudioCaptureRing allocation until the feature is actually
    // activated (setActive(true) or requestDecisionNow). The ring is ~12.3 MB
    // (8s @ 192kHz stereo); lazy allocation cuts More-Phi's baseline memory
    // footprint by ~60% when SonicMaster is not in use.
    ring_.reset();

    // Host-rate window length equivalent to the 44.1k model segment.
    // These model buffers (~4.2 MB total) are always allocated — they're needed
    // for resampling and inference regardless of whether capture is active.
    const std::size_t hostFrames = static_cast<std::size_t>(
        std::llround(kSonicMasterSegmentFrames * sampleRate_ / 44100.0));
    captureL_.assign(hostFrames, 0.0f);
    captureR_.assign(hostFrames, 0.0f);
    modelL_.assign(kSonicMasterSegmentFrames, 0.0f);
    modelR_.assign(kSonicMasterSegmentFrames, 0.0f);
    interleaved_.assign(2 * kSonicMasterSegmentFrames, 0.0f);
    decision_.fill(0.0f);

    consecutiveFailures_.store(0, std::memory_order_relaxed);
    lastPlanId_ = 0;
    nextPlanId_ = 1;
    stopRequested_.store(false, std::memory_order_release);
    prepared_.store(true, std::memory_order_release);
    status_.store(isAvailable() ? Status::CollectingAudio : Status::Disabled,
                  std::memory_order_release);

    thread_ = std::thread([this] { analysisLoop(); });
}

void SonicMasterAnalysisEngine::release() noexcept
{
    const bool wasPrepared = prepared_.load(std::memory_order_relaxed);
    stopRequested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    prepared_.store(false, std::memory_order_release);
    if (wasPrepared)
        status_.store(Status::Disabled, std::memory_order_release);
    ring_.reset();
}

void SonicMasterAnalysisEngine::capture(const float* left, const float* right,
                                        std::size_t n) noexcept
{
    if (!active_.load(std::memory_order_relaxed)
        || !prepared_.load(std::memory_order_relaxed)
        || ring_ == nullptr || left == nullptr || right == nullptr)
        return;
    ring_->write(left, right, n);
}

void SonicMasterAnalysisEngine::analysisLoop() noexcept
{
    using clock = std::chrono::steady_clock;
    auto nextDeadline = clock::now();

    while (!stopRequested_.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, nextDeadline,
                           [this] { return stopRequested_.load(std::memory_order_acquire); });
        }
        if (stopRequested_.load(std::memory_order_acquire)) break;

        nextDeadline += std::chrono::milliseconds(
            static_cast<int>(config_.analysisIntervalSeconds * 1000.0));

        // ponytail: keep the availability cache warm on this background thread so
        // the editor's isAvailable() (message thread) never blocks on a /health
        // probe to a dead server. Throttled internally to ~5 s inside refreshProbe.
        if (source_ != nullptr)
            source_->refreshProbe();

        if (!active_.load(std::memory_order_relaxed)) continue;
        runCycle();
    }
}

bool SonicMasterAnalysisEngine::runOneCycleForTest() noexcept
{
    if (!prepared_.load(std::memory_order_relaxed)) return false;
    ensureRing();  // PERF-MEM: lazy ring allocation
    active_.store(true, std::memory_order_relaxed);
    return runCycle();
}

bool SonicMasterAnalysisEngine::runCycle() noexcept
{
    ensureRing();  // PERF-MEM: safety — ring may not be allocated yet if called from test path
    if (ring_ == nullptr || source_ == nullptr) return false;
    if (!isAvailable() || !active_.load(std::memory_order_relaxed))
    {
        status_.store(Status::Disabled, std::memory_order_release);
        return false;
    }

    // AUDIT-1: serialize the capture/infer scratch path. runCycle runs on the
    // analysis thread; requestDecisionNow runs on the message thread (MCP tool
    // sonicmaster_decision). Both touch captureL_/captureR_/modelL_/modelR_/
    // interleaved_/decision_. Without this lock, concurrent on-demand + cycle
    // inferences corrupt each other's input nondeterministically.
    std::lock_guard<std::mutex> inferLock(inferMutex_);

    const std::size_t hostFrames = captureL_.size();
    const std::size_t got = ring_->readNewest(hostFrames, captureL_.data(), captureR_.data());
    if (got < hostFrames)
    {
        status_.store(Status::CollectingAudio, std::memory_order_release);
        return false;
    }
    // AUDIT-IX-8: record the steady-clock instant the capture window ended, so a
    // plan applied much later (after analysisInterval + inference latency + the
    // async hop) can be discarded if it no longer describes the current audio.
    captureTimeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    // Resample host-rate -> 44.1k if needed.
    if (std::abs(sampleRate_ - 44100.0) < 0.5)
    {
        std::copy_n(captureL_.data(), kSonicMasterSegmentFrames, modelL_.data());
        std::copy_n(captureR_.data(), kSonicMasterSegmentFrames, modelR_.data());
    }
    else
    {
        resampleLinear(captureL_.data(), hostFrames, kSonicMasterSegmentFrames, modelL_.data());
        resampleLinear(captureR_.data(), hostFrames, kSonicMasterSegmentFrames, modelR_.data());
    }

    // Peak-normalize to -1 dBFS so the model sees a consistent operating level.
    // AUDIT-7: this DESTROYS absolute loudness information — a -23 LUFS track
    // and a -8 LUFS track both arrive at the model at -1 dBFS peak. The model
    // cannot infer absolute LUFS from the waveform; the only loudness signal
    // it gets is the target_lufs param the CALLER supplies. Do NOT treat any
    // model "loudness analysis" output as a measurement of the input — it is a
    // function of the caller's target. Consistent with training preprocessing.
    const float peak = std::max(peakAbs(modelL_.data(), kSonicMasterSegmentFrames),
                                peakAbs(modelR_.data(), kSonicMasterSegmentFrames));
    const float gain = peak > 1e-9f ? (0.891f / peak) : 1.0f; // 10^(-1/20) ~ 0.891
    for (std::size_t i = 0; i < kSonicMasterSegmentFrames; ++i)
    {
        interleaved_[2 * i + 0] = modelL_[i] * gain;
        interleaved_[2 * i + 1] = modelR_[i] * gain;
    }

    if (!source_->infer(interleaved_.data(), decision_.data(), decision_.size()))
    {
        const int fails = consecutiveFailures_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fails >= config_.consecutiveFailureLimit)
        {
            active_.store(false, std::memory_order_relaxed);
            status_.store(Status::ErrorAutoDisabled, std::memory_order_release);
        }
        return false;
    }
    consecutiveFailures_.store(0, std::memory_order_relaxed);

    ValidatedNeuralMasteringPlan plan {};
    if (!decodeSonicMasterDecision(decision_.data(), decision_.size(), sampleRate_, plan))
        return false;
    plan.sourcePlanId = nextPlanId_++;
    plan.capturedAtSteadyClockNs = captureTimeNs_;  // stamp capture instant for staleness guard

    // Safety gate. Wrap the decoded plan in a candidate the policy validates.
    NeuralMasteringRuntimeState runtime {};
    runtime.sampleRate = sampleRate_;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;

    const auto candidate = makeSafetyCandidate(plan,
                                               NeuralMasteringRuntimeMode::Background,
                                               config_.confidenceFloor,
                                               safetyPolicy_);
    const auto verdict = safetyPolicy_.validate(candidate, runtime);
    if (!verdict.accepted)
    {
        status_.store(Status::HeldLowConfidence, std::memory_order_release);
        return false;
    }

    // Apply the policy verdict, not the raw decoded plan. The verdict carries
    // per-plan delta projection (for example EQ/loudness slew limits).
    applyRamped(verdict.plan);
    lastPlanId_ = verdict.plan.sourcePlanId;
    status_.store(Status::Applied, std::memory_order_release);
    return true;
}

bool SonicMasterAnalysisEngine::requestDecisionNow(float targetLufs,
                                                    ValidatedNeuralMasteringPlan& outPlan,
                                                    float* outRawDecision,
                                                    std::size_t outRawCapacity) noexcept
{
    // On-demand path for the AI assistant's sonicmaster_decision MCP tool.
    // Mirrors runCycle() but: (a) takes a target-LUFS argument, (b) does NOT
    // require active_ to be set, (c) does NOT apply the plan or touch status_,
    // (d) returns the decoded plan + raw decision for the caller to act on.
    ensureRing();  // PERF-MEM: lazy ring allocation
    if (ring_ == nullptr || source_ == nullptr || !isAvailable())
        return false;

    // AUDIT-1: see runCycle() — serialize against the analysis thread's scratch use.
    std::lock_guard<std::mutex> inferLock(inferMutex_);

    const std::size_t hostFrames = captureL_.size();
    const std::size_t got = ring_->readNewest(hostFrames, captureL_.data(), captureR_.data());
    if (got < hostFrames)
        return false;  // not enough audio captured yet

    if (std::abs(sampleRate_ - 44100.0) < 0.5)
    {
        std::copy_n(captureL_.data(), kSonicMasterSegmentFrames, modelL_.data());
        std::copy_n(captureR_.data(), kSonicMasterSegmentFrames, modelR_.data());
    }
    else
    {
        resampleLinear(captureL_.data(), hostFrames, kSonicMasterSegmentFrames, modelL_.data());
        resampleLinear(captureR_.data(), hostFrames, kSonicMasterSegmentFrames, modelR_.data());
    }

    const float peak = std::max(peakAbs(modelL_.data(), kSonicMasterSegmentFrames),
                                peakAbs(modelR_.data(), kSonicMasterSegmentFrames));
    const float gain = peak > 1e-9f ? (0.891f / peak) : 1.0f;
    for (std::size_t i = 0; i < kSonicMasterSegmentFrames; ++i)
    {
        interleaved_[2 * i + 0] = modelL_[i] * gain;
        interleaved_[2 * i + 1] = modelR_[i] * gain;
    }

    // Propagate the requested target LUFS to the inference source (the HTTP
    // source sends it as a query param; the ONNX source ignores it).
    source_->setTargetLufs(targetLufs);

    if (!source_->infer(interleaved_.data(), decision_.data(), decision_.size()))
        return false;

    if (outRawDecision != nullptr && outRawCapacity >= kSonicMasterDecisionWidth)
        std::copy_n(decision_.data(), kSonicMasterDecisionWidth, outRawDecision);

    ValidatedNeuralMasteringPlan plan {};
    if (!decodeSonicMasterDecision(decision_.data(), decision_.size(), sampleRate_, plan))
        return false;
    plan.sourcePlanId = nextPlanId_++;

    NeuralMasteringRuntimeState runtime {};
    runtime.sampleRate = sampleRate_;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;

    auto decisionPolicy = safetyPolicy_;
    const auto candidate = makeSafetyCandidate(plan,
                                               NeuralMasteringRuntimeMode::MessageThread,
                                               config_.confidenceFloor,
                                               decisionPolicy);
    const auto verdict = decisionPolicy.validate(candidate, runtime);
    if (!verdict.accepted)
        return false;

    outPlan = verdict.plan;
    return true;
}

void SonicMasterAnalysisEngine::applyRamped(const ValidatedNeuralMasteringPlan& plan) noexcept
{
    if (applicationEngine_ == nullptr) return;

    // AUDIT-IX-8: staleness guard. If the plan's capture instant is older than
    // the staleness budget, discard it instead of applying against audio it no
    // longer describes. Budget = analysisInterval + inference latency + async
    // hop slack. Plans from legacy producers carry capturedAtSteadyClockNs == 0
    // (requestDecisionNow / unit tests) and skip the check.
    if (plan.capturedAtSteadyClockNs != 0)
    {
        const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto ageNs = static_cast<std::uint64_t>(nowNs - static_cast<std::int64_t>(plan.capturedAtSteadyClockNs));
        constexpr std::uint64_t kStalenessBudgetNs =
            10u * 1000u * 1000u * 1000u; // 10 s: 3 s interval + ~3 s latency + slack
        if (ageNs > kStalenessBudgetNs)
        {
            status_.store(Status::HeldLowConfidence, std::memory_order_release);
            return;
        }
    }

    // If we have a JUCE message manager and we're NOT on the message thread
    // (i.e. the real analysis-thread path), hop to the message thread so the
    // DSP setter semantics hold. In the unit-test path there is no message
    // manager, so apply synchronously on the calling thread.
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && !mm->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync(
            [engine = applicationEngine_, p = plan]() { engine->applyValidatedPlan(p); });
    }
    else
    {
        applicationEngine_->applyValidatedPlan(plan);
    }
}

SonicMasterMeasurementSnapshot SonicMasterAnalysisEngine::getLiveMeasurements() const noexcept
{
    // AUDIT-FIX-1: pull GENUINE classical measurements from AutoMasteringEngine's
    // already-running meters (LUFSMeter is a true BS.1770-4 K-weighting + gated
    // integrator; TruePeakEstimator is a 4x polyphase ISP meter; the spectrum /
    // stereo analyzers are real FFT + correlation). These are MEASUREMENTS of the
    // input — distinct from the model ESTIMATE, which is peak-normalized and so
    // cannot see input loudness. The assistant should report both, labeled.
    SonicMasterMeasurementSnapshot out {};
    if (applicationEngine_ == nullptr)
        return out;

    out.lufsIntegrated = applicationEngine_->getLUFSIntegrated();
    out.lufsShortTerm  = applicationEngine_->getLUFSShortTerm();
    out.lufsMomentary  = applicationEngine_->getLUFSMomentary();
    out.lra            = applicationEngine_->getLRA();
    out.truePeakDbtp   = applicationEngine_->getTruePeak_dBTP();

    RealtimeSpectrumAnalyzer::SpectrumSnapshot spec;
    if (applicationEngine_->getSpectrumAnalyzer().getSnapshot(spec) && spec.frameIndex > 0)
    {
        out.spectralCentroidHz = spec.spectralCentroid;
        out.spectralTilt       = spec.spectralTilt;
    }

    StereoFieldAnalyzer::StereoFieldSnapshot stereo;
    if (applicationEngine_->getStereoFieldAnalyzer().getSnapshot(stereo) && stereo.frameIndex > 0)
    {
        out.stereoWidth    = stereo.stereoWidth;
        out.correlationMid = stereo.correlation[2]; // mid band, -1..+1
    }

    out.valid = true;
    return out;
}

} // namespace more_phi
