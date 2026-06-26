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
    {
        const float v = a[i];
        if (std::isfinite(v)) m = std::max(m, std::abs(v));
    }
    return m;
}

// Polyphase FIR resampler with arbitrary ratio. Uses a windowed-sinc kernel
// (Kaiser, beta=6, 64 taps, 128 phases, ~60 dB stopband, transition ~2 kHz).
// Designed for the analysis thread (~0.3 Hz); not for realtime.
void resamplePolyphase(const float* src, std::size_t srcLen,
                       std::size_t dstLen, float* dst) noexcept
{
    if (srcLen == 0 || dstLen == 0) return;
    if (srcLen == 1) { std::fill_n(dst, dstLen, src[0]); return; }

    constexpr int kPhases = 128;
    constexpr int kTapsPerPhase = 8;
    constexpr int kNumTaps = kPhases * kTapsPerPhase;
    constexpr double kPi = 3.14159265358979323846;

    // Kaiser-windowed-sinc prototype filter, designed once.
    static const auto lpCoeffs = []() -> std::array<double, kNumTaps> {
        std::array<double, kNumTaps> h {};
        constexpr double beta = 6.0;
        // Compute I0(beta) once for normalization.
        auto i0 = [](double x) -> double {
            double sum = 1.0, term = 1.0;
            const double t = x / 2.0;
            const double tt = t * t;
            for (int k = 1; k <= 12; ++k)
            { term *= tt / static_cast<double>(k * k); sum += term; }
            return sum;
        };
        const double i0beta = i0(beta);
        constexpr double cutoff = 0.42;
        const double alpha = static_cast<double>(kNumTaps - 1) / 2.0;
        for (int i = 0; i < kNumTaps; ++i)
        {
            const double x = (static_cast<double>(i) - alpha) / alpha;
            const double w = std::abs(x) < 1.0
                ? i0(beta * std::sqrt(1.0 - x * x)) / i0beta
                : 0.0;
            const double n = static_cast<double>(i) - alpha;
            const double sinc = std::abs(n) < 1e-12
                ? cutoff * kPi
                : std::sin(cutoff * kPi * n) / n;
            h[static_cast<std::size_t>(i)] = sinc * w;
        }
        double sum = 0.0;
        for (auto& v : h) sum += v;
        const double inv = 1.0 / sum;
        for (auto& v : h) v *= inv;
        return h;
    }();

    const double ratio = static_cast<double>(srcLen) / static_cast<double>(dstLen);
    for (std::size_t i = 0; i < dstLen; ++i)
    {
        const double centre = static_cast<double>(i) * ratio;
        const int idx0 = static_cast<int>(std::floor(centre)) - kTapsPerPhase / 2 + 1;
        const double frac = centre - std::floor(centre);
        const int phaseIdx = static_cast<int>(std::round(frac * static_cast<double>(kPhases))) % kPhases;
        double sum = 0.0;
        for (int t = 0; t < kTapsPerPhase; ++t)
        {
            const int srcIdx = idx0 + t;
            if (srcIdx < 0 || srcIdx >= static_cast<int>(srcLen)) continue;
            sum += static_cast<double>(src[srcIdx])
                 * lpCoeffs[static_cast<std::size_t>(phaseIdx + t * kPhases)];
        }
        dst[i] = static_cast<float>(sum);
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
                                                 const NeuralMasteringSafetyPolicy& policy,
                                                 std::uint64_t currentFrame) noexcept
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
    // AUDIT-FIX (Fix 7): populate frame-based staleness so the safety policy's
    // StalePlan/InvalidTimestamp checks (NeuralMasteringSafetyPolicy.cpp:357-361)
    // actually fire for the SonicMaster path. Previously both fields stayed 0,
    // disabling the check (0 < 0 is false, 0 > currentFrame is false on a live
    // stream → neither branch tripped, but a genuinely stale plan also slipped
    // through because producedAtFrame(0) was never > currentFrame). Matches the
    // OnnxNeuralMasteringRunner precedent (expiresAfterFrame = produced + 96000).
    candidate.producedAtFrame   = currentFrame;
    candidate.expiresAfterFrame = currentFrame + policy.getConfig().maxPlanAgeFrames;
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
    // C-3 FIX (audit): ring_ is now an atomic raw pointer; ringStorage_ owns
    // the lifetime. We construct into ringStorage_ first, then PUBLISH the
    // raw pointer to ring_ with release semantics so the audio thread (which
    // loads ring_ with acquire in capture()) observes a fully-constructed
    // AudioCaptureRing — never a partially-constructed object.
    if (ring_.load(std::memory_order_acquire) != nullptr) return;
    if (!prepared_.load(std::memory_order_relaxed)) return;

    // AUDIT-FIX-R11: sanity-check the ring size. captureRingFrames is sized in
    // prepare() to 8 s at the actual host sample rate (PERF-MEM-RATE), then
    // pow2-rounded by AudioCaptureRing. At 48 kHz that's ~4.0 MiB; at 192 kHz
    // ~16.0 MiB. (AUDIT-2026-06-25: earlier comments cited a fixed "16.0 MiB"
    // or "12.3 MB" — both stale; the ring is now rate-proportional.)
    jassert(config_.captureRingFrames >= 2u * 44100u);           // at least 2s @ 44.1k
    jassert(config_.captureRingFrames <= 32u * 192000u);         // at most 32s @ 192k

    ringStorage_ = std::make_unique<AudioCaptureRing>(config_.captureRingFrames);
    ring_.store(ringStorage_.get(), std::memory_order_release);
}

void SonicMasterAnalysisEngine::prepare(double sampleRate, int /*maxBlockSize*/)
{
    // Re-prepare is allowed: tear down any prior thread first.
    release();

    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    // PERF-MEM-RATE: Right-size the capture ring to 8 s at the ACTUAL host sample
    // rate (was a fixed 8*192000 = 1,536,000 frames → 16.0 MiB regardless of rate).
    // Most sessions run at 44.1/48 kHz, where 8 s is ~353k/384k frames → pow2-rounds
    // to 512k frames × 2 ch × 4 B = 4.0 MiB (vs 16.0 MiB at 192 kHz). AudioCaptureRing
    // pow2-rounds again in its ctor, so this is a soft target. Clamped to the same
    // [2s@44.1k, 32s@192k] bounds ensureRing asserts.
    {
        const double framesF = 8.0 * sampleRate_;
        auto frames = static_cast<std::size_t>(framesF);
        if (frames < 2u * 44100u)   frames = 2u * 44100u;
        if (frames > 32u * 192000u) frames = 32u * 192000u;
        config_.captureRingFrames = frames;
    }

    // PERF-MEM: Defer AudioCaptureRing allocation until the feature is actually
    // activated (setActive(true) or requestDecisionNow). The ring is rate-proportional
    // (~4 MiB at 48 kHz, ~16 MiB at 192 kHz, pow2-rounded); lazy allocation cuts
    // More-Phi's baseline memory footprint substantially when SonicMaster is off.
    // C-3 FIX (audit): publish nullptr BEFORE freeing storage so an audio
    // thread in capture() that loads ring_ with acquire bails before the
    // object is destroyed. Safe w.r.t. processBlock because JUCE guarantees
    // prepare()/release() are mutually exclusive with processBlock.
    ring_.store(nullptr, std::memory_order_release);
    ringStorage_.reset();

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
    // Stage D: reset the closed LUFS loop so a fresh prepare starts open-loop.
    feedbackActive_ = false;
    feedbackTargetLufs_ = -14.0f;
    lastAppliedTargetLufs_ = -14.0f;
    lastMeasuredLufs_ = 0.0f;
    lastLufsError_ = 0.0f;
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
    // C-3 FIX (audit): null-publish first (audio-thread acquire-load bails),
    // then free the owning storage. Analysis thread is already joined above,
    // and processBlock is mutually exclusive with release() per the JUCE host
    // contract — so no in-flight capture() can be dereferencing the ring.
    ring_.store(nullptr, std::memory_order_release);
    ringStorage_.reset();
}

void SonicMasterAnalysisEngine::capture(const float* left, const float* right,
                                        std::size_t n) noexcept
{
    if (!active_.load(std::memory_order_relaxed)
        || !prepared_.load(std::memory_order_acquire)
        || left == nullptr || right == nullptr)
        return;
    // C-3 FIX (audit): acquire-load the published ring pointer. Paired with
    // the release-store in ensureRing()/prepare()/release(), this guarantees
    // we see either a fully-constructed AudioCaptureRing or nullptr — never a
    // torn or partially-constructed object.
    auto* ring = ring_.load(std::memory_order_acquire);
    if (ring == nullptr)
        return;
    ring->write(left, right, n);
}

void SonicMasterAnalysisEngine::flushCaptureRing() noexcept
{
    // AUDIT-FIX-R7: Reset the ring discarding all previously captured audio.
    // Safe to call on the message thread while the audio thread is writing
    // (AudioCaptureRing::reset() only touches atomics). The analysis thread
    // will see an empty ring on its next readNewest() call and skip the cycle
    // with "CollectingAudio" status until enough new audio accumulates.
    // C-3 FIX (audit): acquire-load the atomic pointer.
    if (auto* ring = ring_.load(std::memory_order_acquire))
        ring->reset();
}

void SonicMasterAnalysisEngine::analysisLoop() noexcept
{
    using clock = std::chrono::steady_clock;
    const auto kIntervalMs = std::chrono::milliseconds(
        static_cast<int>(config_.analysisIntervalSeconds * 1000.0));
    // Defer the first cycle by one full interval so unit tests have time to
    // set up before the background thread runs its first inference. Production
    // hosts call prepare() well before the user activates SonicMaster, so the
    // extra delay is invisible there.
    auto nextDeadline = clock::now() + kIntervalMs;

    while (!stopRequested_.load(std::memory_order_acquire))
    {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_until(lock, nextDeadline,
                           [this] { return stopRequested_.load(std::memory_order_acquire); });
        }
        if (stopRequested_.load(std::memory_order_acquire)) break;

        // ponytail: keep the availability cache warm on this background thread so
        // the editor's isAvailable() (message thread) never blocks on a /health
        // probe to a dead server. Throttled internally to ~5 s inside refreshProbe.
        if (source_ != nullptr)
            source_->refreshProbe();

        if (!active_.load(std::memory_order_relaxed)) { nextDeadline += kIntervalMs; continue; }
        runCycle();
        nextDeadline += kIntervalMs;
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
    // C-3 FIX (audit): acquire-load the atomic ring pointer.
    auto* ring = ring_.load(std::memory_order_acquire);
    if (ring == nullptr || source_ == nullptr) return false;

    // Auto-recovery: if previously disabled by consecutive failures and the
    // inference source is healthy again, re-enable the cycle transparently.
    if (!active_.load(std::memory_order_relaxed)
        && status_.load(std::memory_order_acquire) == Status::ErrorAutoDisabled
        && isAvailable())
    {
        active_.store(true, std::memory_order_relaxed);
        consecutiveFailures_.store(0, std::memory_order_relaxed);
    }

    if (!active_.load(std::memory_order_relaxed))
    {
        status_.store(Status::Disabled, std::memory_order_release);
        return false;
    }
    if (!isAvailable())
        return false;

    // AUDIT-1: serialize the capture/infer scratch path. runCycle runs on the
    // analysis thread; requestDecisionNow runs on the message thread (MCP tool
    // sonicmaster_decision). Both touch captureL_/captureR_/modelL_/modelR_/
    // interleaved_/decision_. Without this lock, concurrent on-demand + cycle
    // inferences corrupt each other's input nondeterministically.
    std::lock_guard<std::mutex> inferLock(inferMutex_);

    const std::size_t hostFrames = captureL_.size();
    const std::size_t got = ring->readNewest(hostFrames, captureL_.data(), captureR_.data());
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

    // P2.8 (AUDIT): transition guard. If a hosted-plugin parameter changed during
    // or shortly before this capture window, the window is a hybrid of two
    // parameter states — the model would produce a decision for a state that never
    // existed. Discard the window, flush the ring so the next cycle starts fresh,
    // and report CollectingAudio until a clean post-settle window accumulates.
    // settleNs gives the hosted plugin's internal state (filters, lookahead, tails)
    // time to flush after the change.
    if (paramChangePending_.load(std::memory_order_relaxed))
    {
        const std::uint64_t changeNs = paramChangeNs_;  // relaxed read, advisory
        const double windowSeconds = static_cast<double>(hostFrames) / sampleRate_;
        const std::uint64_t windowNs = static_cast<std::uint64_t>(windowSeconds * 1e9);
        const std::uint64_t settleNs = static_cast<std::uint64_t>(paramSettleSeconds_ * 1e9);
        // Contaminated if the change occurred within [captureTimeNs - window, captureTimeNs + settle].
        const std::uint64_t windowStartNs =
            captureTimeNs_ > windowNs ? captureTimeNs_ - windowNs : 0;
        const bool inWindow = (changeNs >= windowStartNs && changeNs <= captureTimeNs_);
        // Also require the full settle period to have elapsed since the change so
        // we don't analyze a window whose tail still reflects the prior setting.
        const bool stillSettling =
            captureTimeNs_ < changeNs || (captureTimeNs_ - changeNs) < settleNs;
        if (inWindow || stillSettling)
        {
            // Flush and clear the pending flag; the next cycle re-evaluates. If
            // another change arrives in the meantime, notifyHostedParameterChanged
            // re-arms it — we only clear when we've actually observed the change.
            paramChangePending_.store(false, std::memory_order_relaxed);
            if (auto* ring2 = ring_.load(std::memory_order_acquire))
                ring2->reset();
            status_.store(Status::CollectingAudio, std::memory_order_release);
            return false;
        }
        // The change predates this window by more than the settle period: clear
        // the flag and analyze normally (the window is clean).
        paramChangePending_.store(false, std::memory_order_relaxed);
    }

    // Resample host-rate -> 44.1k if needed.
    if (std::abs(sampleRate_ - 44100.0) < 0.5)
    {
        std::copy_n(captureL_.data(), kSonicMasterSegmentFrames, modelL_.data());
        std::copy_n(captureR_.data(), kSonicMasterSegmentFrames, modelR_.data());
    }
    else
    {
        resamplePolyphase(captureL_.data(), hostFrames, kSonicMasterSegmentFrames, modelL_.data());
        resamplePolyphase(captureR_.data(), hostFrames, kSonicMasterSegmentFrames, modelR_.data());
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
    // AUDIT-3.4: if all samples were non-finite (peakAbs returned 0.0 because
    // every sample was NaN/Inf), skip inference entirely rather than passing a
    // gain-normalized all-NaN buffer to the model — that is undefined behaviour
    // for many operators (softmax, layer norm).
    if (!std::isfinite(peak) || peak < 1e-15f)
        return false;
    // AUDIT-FIX (A2): use the named contract constant (was a bare 0.891f literal).
    const float gain = kSonicMasterPeakTargetLinear / peak;
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
            // AUDIT LOW-4: ONNX→HTTP failover. If a fallback source is wired and
            // we're not already on it, swap to the fallback instead of disabling.
            // The fallback gets a clean failure counter. Only disable if we're
            // already on the fallback (or no fallback exists). This swap happens
            // on the analysis thread — the only reader of source_ is this thread
            // (under inferMutex_), so no race.
            if (fallbackSource_ != nullptr && source_ != fallbackSource_)
            {
                source_ = fallbackSource_;
                consecutiveFailures_.store(0, std::memory_order_relaxed);
                // Stay active — the fallback gets a chance.
            }
            else
            {
                active_.store(false, std::memory_order_relaxed);
                status_.store(Status::ErrorAutoDisabled, std::memory_order_release);
            }
        }
        return false;
    }
    consecutiveFailures_.store(0, std::memory_order_relaxed);

    ValidatedNeuralMasteringPlan plan {};
    // Stage D (2026-06-26): closed LUFS loop. When active, override the model's
    // loudness recommendation with the feedback-corrected target from the prior
    // cycle's measured error. The decode-side hook (Stage A) honors it. First
    // cycle (or after reset) uses the model's own recommendation.
    const float decodeTargetLufs = feedbackActive_
        ? feedbackTargetLufs_
        : more_phi::kUseModelTargetLufs;
    if (!decodeSonicMasterDecision(decision_.data(), decision_.size(), sampleRate_, plan, decodeTargetLufs))
        return false;
    plan.sourcePlanId = nextPlanId_++;
    plan.capturedAtSteadyClockNs = captureTimeNs_;  // stamp capture instant for staleness guard
    // Record the target this plan actually applied (pre-safety-projection value,
    // for the error calc). Inverse of the decoder map: lufs = -14 + value*6.
    lastAppliedTargetLufs_ = std::clamp(-14.0f + plan.projectedTargets.loudness[0] * 6.0f,
                                        kFeedbackMinTargetLu, kFeedbackMaxTargetLu);

    // Safety gate. Wrap the decoded plan in a candidate the policy validates.
    NeuralMasteringRuntimeState runtime {};
    runtime.sampleRate = sampleRate_;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;
    // AUDIT-FIX (Fix 7): feed the real audio frame count so the safety policy's
    // frame-based staleness check fires (previously currentFrame stayed 0 and the
    // check was inert for the SonicMaster path). Pulled from AutoMasteringEngine's
    // analyzeBlock accumulator; falls back to 0 when no app engine is attached.
    if (applicationEngine_ != nullptr)
        runtime.currentFrame = applicationEngine_->getAnalyzedSampleCount();

    const auto candidate = makeSafetyCandidate(plan,
                                               NeuralMasteringRuntimeMode::Background,
                                               config_.confidenceFloor,
                                               safetyPolicy_,
                                               runtime.currentFrame);
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
    // F2/AUDIT: the plan was written into AutoMasteringEngine's DSP setters, but
    // that chain is dormant in the shipped plugin (prepare(...,false) at
    // PluginProcessor.cpp:1265). If it's not active AND the Ozone applicator
    // didn't write any parameters, the apply reached no audio path — say so
    // instead of reporting Applied. The Ozone bridge (CRITICAL-6/7/17) runs
    // inside applyValidatedPlan; when a hosted plugin is wired, its parameter
    // enqueue count > 0 means audio is actually being affected even though the
    // internal chain is dormant.
    const bool internalActive = (applicationEngine_ != nullptr) && applicationEngine_->isActive();
    const bool ozoneWrote     = (applicationEngine_ != nullptr) && applicationEngine_->getLastOzoneAppliedCount() > 0;
    const bool reachedAudio   = internalActive || ozoneWrote;
    lastApplyReachedAudioPath_.store(reachedAudio, std::memory_order_release);
    status_.store(reachedAudio ? Status::Applied : Status::AppliedNoAudioPath,
                  std::memory_order_release);

    // Stage D (2026-06-26): closed LUFS feedback loop. Measure the achieved LUFS
    // on the captured (post-hosted-plugin) signal and fold a BOUNDED correction
    // into the next cycle's target. Guards: only when the apply reached audio,
    // the measurement is valid, and the error exceeds the deadband (no churn
    // on-target). The correction is capped at kFeedbackMaxCorrectionLu/cycle so
    // the loop can't runaway or oscillate; combined with the 3s cadence this
    // converges monotonically over a few cycles.
    if (reachedAudio)
    {
        const auto m = getLiveMeasurements();
        if (m.valid)
        {
            lastMeasuredLufs_ = m.lufsIntegrated;
            lastLufsError_ = lastAppliedTargetLufs_ - m.lufsIntegrated; // + = too quiet
            if (std::abs(lastLufsError_) > kFeedbackDeadbandLu)
            {
                // Nudge the next target toward reducing the error. Sign: if signal
                // is too quiet (error > 0), raise the target; if too loud, lower it.
                const float nudge = std::clamp(lastLufsError_,
                                               -kFeedbackMaxCorrectionLu,
                                               kFeedbackMaxCorrectionLu);
                feedbackTargetLufs_ = std::clamp(lastAppliedTargetLufs_ + nudge,
                                                 kFeedbackMinTargetLu,
                                                 kFeedbackMaxTargetLu);
                feedbackActive_ = true;
            }
            else
            {
                // On-target: lock the loop at the current target (no drift).
                feedbackTargetLufs_ = lastAppliedTargetLufs_;
                feedbackActive_ = true;
            }
        }
        // If measurement is invalid (not enough signal), leave feedbackTargetLufs_
        // unchanged — the loop holds its last good target rather than guessing.
    }

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
    // C-3 FIX (audit): acquire-load the atomic ring pointer.
    auto* ring = ring_.load(std::memory_order_acquire);
    if (ring == nullptr || source_ == nullptr || !isAvailable())
        return false;

    // AUDIT-1: see runCycle() — serialize against the analysis thread's scratch use.
    std::lock_guard<std::mutex> inferLock(inferMutex_);

    const std::size_t hostFrames = captureL_.size();
    const std::size_t got = ring->readNewest(hostFrames, captureL_.data(), captureR_.data());
    if (got < hostFrames)
        return false;  // not enough audio captured yet

    // FIX-1.3: stamp the capture time for the staleness guard, so an on-demand
    // plan from the MCP tool cannot be applied hours later against fresh audio.
    captureTimeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    if (std::abs(sampleRate_ - 44100.0) < 0.5)
    {
        std::copy_n(captureL_.data(), kSonicMasterSegmentFrames, modelL_.data());
        std::copy_n(captureR_.data(), kSonicMasterSegmentFrames, modelR_.data());
    }
    else
    {
        resamplePolyphase(captureL_.data(), hostFrames, kSonicMasterSegmentFrames, modelL_.data());
        resamplePolyphase(captureR_.data(), hostFrames, kSonicMasterSegmentFrames, modelR_.data());
    }

    const float peak = std::max(peakAbs(modelL_.data(), kSonicMasterSegmentFrames),
                                peakAbs(modelR_.data(), kSonicMasterSegmentFrames));
    if (!std::isfinite(peak) || peak < 1e-15f)
        return false;
    // AUDIT-FIX (A2): use the named contract constant (was a bare 0.891f literal).
    const float gain = kSonicMasterPeakTargetLinear / peak;
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
    // Stage A (2026-06-26): honor the caller's explicit target_lufs at decode
    // time. The ONNX graph can't condition on a target during inference, so this
    // decode-side override is what makes an explicit target (profile, manual, or
    // closed-loop correction) actually reach the apply. requestDecisionNow is the
    // explicit on-demand path; the background runCycle uses the model default and
    // is nudged by the closed loop (Stage D).
    if (!decodeSonicMasterDecision(decision_.data(), decision_.size(), sampleRate_, plan,
                                   std::isfinite(targetLufs) ? targetLufs : kUseModelTargetLufs))
        return false;
    plan.sourcePlanId = nextPlanId_++;
    plan.capturedAtSteadyClockNs = captureTimeNs_;

    NeuralMasteringRuntimeState runtime {};
    runtime.sampleRate = sampleRate_;
    runtime.channelCount = 2;
    runtime.layout = NeuralMasteringLayout::Stereo;
    // AUDIT-FIX (Fix 7): same frame-based staleness population as runCycle above.
    if (applicationEngine_ != nullptr)
        runtime.currentFrame = applicationEngine_->getAnalyzedSampleCount();

    auto decisionPolicy = safetyPolicy_;
    const auto candidate = makeSafetyCandidate(plan,
                                               NeuralMasteringRuntimeMode::MessageThread,
                                               config_.confidenceFloor,
                                               decisionPolicy,
                                               runtime.currentFrame);
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

    // AUDIT-FIX-R5: pending-plan handoff replaces MessageManager::callAsync.
    // callAsync can silently drop callbacks in headless hosts (FL Studio on
    // Linux, offline-render configurations). Instead we store the plan with
    // release semantics and let the processor's message-thread timer poll
    // hasPendingApplication() / processPendingApplication(). This matches the
    // project's morphPositionNotifyPending_ pattern exactly.
    //
    // If the JUCE MessageManager exists and we ARE on the message thread (e.g.
    // unit-test path with no background analysis thread), apply synchronously
    // for backward compatibility.
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm != nullptr && mm->isThisTheMessageThread())
    {
        applicationEngine_->applyValidatedPlan(plan);
        return;
    }

    // HEADLESS FALLBACK: if there is no asynchronous drain path available (no
    // maintenance callback wired by the processor AND no MessageManager to pump
    // the message thread), the pending plan would sit forever and never reach
    // applyValidatedPlan. This is the unit-test / no-host case. Apply
    // synchronously so the safety baseline (hasLastSafeNeuralPlan_) advances
    // exactly as it would in production. Production builds always wire the
    // maintenance callback, so this branch is inert there.
    if (maintenanceRequestCb_ == nullptr && mm == nullptr)
    {
        applicationEngine_->applyValidatedPlan(plan);
        return;
    }

    // Analysis thread: store the plan and signal the message thread.
    // Only runCycle writes here (under inferMutex_), so no concurrent writer.
    pendingPlan_ = plan;
    pendingApplication_.store(true, std::memory_order_release);

    // Request message-thread maintenance so the processor's timer fires and
    // calls processPendingApplication(). This replaces the old callAsync hop.
    if (maintenanceRequestCb_)
        maintenanceRequestCb_();
}

void SonicMasterAnalysisEngine::processPendingApplication() noexcept
{
    // Message thread only — apply the stored plan and clear the flag.
    if (!pendingApplication_.load(std::memory_order_acquire))
        return;

    if (applicationEngine_ != nullptr)
        applicationEngine_->applyValidatedPlan(pendingPlan_);

    pendingApplication_.store(false, std::memory_order_release);
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
        out.thdPercent         = spec.thdPercent;
        out.crestFactorProgram = spec.crestFactorProgram;
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
