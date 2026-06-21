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

void SonicMasterAnalysisEngine::prepare(double sampleRate, int /*maxBlockSize*/)
{
    // Re-prepare is allowed: tear down any prior thread first.
    release();

    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    ring_ = std::make_unique<AudioCaptureRing>(config_.captureRingFrames);

    // Host-rate window length equivalent to the 44.1k model segment.
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

        if (!active_.load(std::memory_order_relaxed)) continue;
        runCycle();
    }
}

bool SonicMasterAnalysisEngine::runOneCycleForTest() noexcept
{
    if (!prepared_.load(std::memory_order_relaxed)) return false;
    active_.store(true, std::memory_order_relaxed);
    return runCycle();
}

bool SonicMasterAnalysisEngine::runCycle() noexcept
{
    if (ring_ == nullptr || source_ == nullptr) return false;
    if (!isAvailable() || !active_.load(std::memory_order_relaxed))
    {
        status_.store(Status::Disabled, std::memory_order_release);
        return false;
    }

    const std::size_t hostFrames = captureL_.size();
    const std::size_t got = ring_->readNewest(hostFrames, captureL_.data(), captureR_.data());
    if (got < hostFrames)
    {
        status_.store(Status::CollectingAudio, std::memory_order_release);
        return false;
    }

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

    // Safety gate. Wrap the decoded plan in a candidate the policy validates.
    NeuralMasteringRuntimeState runtime {};
    runtime.sampleRate = sampleRate_;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;

    NeuralMasteringPlanCandidate candidate {};
    candidate.schemaVersion   = kNeuralMasteringPlanSchemaVersion;
    candidate.runtimeMode     = NeuralMasteringRuntimeMode::Background;
    candidate.confidence      = config_.confidenceFloor;
    candidate.evidenceLevel   = plan.evidenceLevel;
    candidate.editableMask    = plan.appliedMask;
    candidate.targets         = plan.projectedTargets;
    candidate.deltas          = plan.projectedTargets;

    const auto verdict = safetyPolicy_.validate(candidate, runtime);
    if (!verdict.accepted)
    {
        status_.store(Status::HeldLowConfidence, std::memory_order_release);
        return false;
    }

    // Apply: bookkeeping + the message-thread hop. applyRamped() hands the
    // decoded plan (already safety-clamped by the decoder) to
    // AutoMasteringEngine::applyValidatedPlan, which re-clamps defensively.
    applyRamped(plan);
    lastPlanId_ = plan.sourcePlanId;
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
    if (ring_ == nullptr || source_ == nullptr || !isAvailable())
        return false;

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

    NeuralMasteringPlanCandidate candidate {};
    candidate.schemaVersion   = kNeuralMasteringPlanSchemaVersion;
    candidate.runtimeMode     = NeuralMasteringRuntimeMode::MessageThread;
    candidate.confidence      = config_.confidenceFloor;
    candidate.evidenceLevel   = plan.evidenceLevel;
    candidate.editableMask    = plan.appliedMask;
    candidate.targets         = plan.projectedTargets;
    candidate.deltas          = plan.projectedTargets;

    if (!safetyPolicy_.validate(candidate, runtime).accepted)
        return false;

    outPlan = plan;
    return true;
}

void SonicMasterAnalysisEngine::applyRamped(const ValidatedNeuralMasteringPlan& plan) noexcept
{
    if (applicationEngine_ == nullptr) return;

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

} // namespace more_phi
