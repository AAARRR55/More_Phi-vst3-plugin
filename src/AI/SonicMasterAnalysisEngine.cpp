/*
 * More-Phi — AI/SonicMasterAnalysisEngine.cpp
 *
 * Implements the realtime neural mastering orchestration loop. See header for
 * the thread model and lifecycle invariants.
 */
#include "AI/SonicMasterAnalysisEngine.h"

#include "Core/AutoMasteringEngine.h"
#include "Core/TonalBalanceExtractor.h"
#include "AI/MasteringTargetCurves.h"

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
//
// AUDIT-FIX (L1-5, 2026-06-29): HF cutoff limitation documented. The 64-tap
// Kaiser(β=6) filter has a Nyquist-anchored cutoff at the OUTPUT sample rate
// (44.1 kHz for model input). When resampling FROM a higher rate (96/192 kHz),
// content above ~20 kHz is attenuated by the transition band. For the mastering
// model this is acceptable (the model was trained on 44.1 kHz data and has no
// representational bandwidth above ~22 kHz), but callers reusing this resampler
// for full-bandwidth analysis should note the ~2 kHz transition zone and the
// ~60 dB stopband floor. The true-peak estimator uses its OWN 4×/64-tap
// polyphase FIR (separate from this one) and is unaffected.
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

SonicMasterAnalysisEngine::CaptureDiagnostics
SonicMasterAnalysisEngine::getCaptureDiagnostics() const noexcept
{
    // CAPTURE-TELEMETRY: all reads are atomic/const — safe from any thread.
    // The ring pointer is acquire-loaded so we see a fully-constructed object
    // or nullptr (never torn); capturedFrames() is itself an atomic acquire-load
    // inside AudioCaptureRing.
    CaptureDiagnostics d;
    d.prepared = prepared_.load(std::memory_order_acquire);
    d.active   = active_.load(std::memory_order_relaxed);
    auto* ring = ring_.load(std::memory_order_acquire);
    d.ringAllocated = (ring != nullptr);
    if (ring != nullptr)
    {
        d.capturedFrames = ring->capturedFrames();
        d.ringCapacity   = ring->capacity();
    }
    d.requiredFrames = captureL_.size();  // host-rate window sized in prepare()
    return d;
}

SonicMasterAnalysisEngine::LastSafetyRejection
SonicMasterAnalysisEngine::getLastSafetyRejection() const noexcept
{
    // DIAGNOSTIC (2026-06-26): the snapshot is written under inferMutex_ (both
    // runCycle and requestDecisionNow hold it across their validate() calls), so
    // we take it here too for a torn-free read. Cheap and uncontended in practice
    // — the only writers are the two inference paths, serialized by the same lock.
    std::lock_guard<std::mutex> lock(inferMutex_);
    LastSafetyRejection out = lastSafetyRejection_;
    out.candidateConfidence = lastRejectedCandidateConfidence_;
    return out;
}

void SonicMasterAnalysisEngine::recordSafetyRejection_(
    const NeuralMasteringValidationResult& verdict, float candidateConfidence) noexcept
{
    // DIAGNOSTIC (2026-06-26): caller (runCycle or requestDecisionNow) already
    // holds inferMutex_ across validate(), so this write is serialized with the
    // getLastSafetyRejection() reader. Copy up to kNeuralMasteringIssueCapacity
    // issues, pick the first non-None as primaryIssue, and flag hardReject if any
    // issue is a hard reject (per isHardRejectNeuralMasteringIssue). MaxDeltaProjected
    // is informational-only and never the reason for a reject, so we skip it when
    // choosing primaryIssue (it can still appear in the issues array for context).
    lastSafetyRejection_ = {};
    lastSafetyRejection_.valid = true;
    lastRejectedCandidateConfidence_ = std::isfinite(candidateConfidence) ? candidateConfidence : 0.0f;
    std::size_t n = verdict.issueCount < lastSafetyRejection_.issues.size()
        ? verdict.issueCount : lastSafetyRejection_.issues.size();
    lastSafetyRejection_.issueCount = n;
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto issue = verdict.issues[i];
        lastSafetyRejection_.issues[i] = issue;
        if (isHardRejectNeuralMasteringIssue(issue))
            lastSafetyRejection_.hardReject = true;
        if (lastSafetyRejection_.primaryIssue == NeuralMasteringValidationIssue::None
            && issue != NeuralMasteringValidationIssue::None
            && issue != NeuralMasteringValidationIssue::MaxDeltaProjected)
        {
            lastSafetyRejection_.primaryIssue = issue;
        }
    }
    // If only MaxDeltaProjected (shouldn't reject, but be defensive) or None made
    // it through, fall back to the first issue verbatim so the field is never None
    // on a recorded rejection.
    if (lastSafetyRejection_.primaryIssue == NeuralMasteringValidationIssue::None && n > 0)
        lastSafetyRejection_.primaryIssue = verdict.issues[0];
}

void SonicMasterAnalysisEngine::clearSafetyRejection_() noexcept
{
    lastSafetyRejection_ = {};
    lastRejectedCandidateConfidence_ = 0.0f;
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

    // CAPTURE-DECOUPLE (2026-06-26): the capture ring is now allocated EAGERLY
    // in prepare() rather than lazily on first setActive(true)/requestDecisionNow.
    // The prior lazy scheme saved ~4 MiB when SonicMaster was off, but it made
    // on-demand capture impossible: capture() bails on a null ring (line ~296),
    // and the ring only materialized when the assistant called requestDecisionNow —
    // by which point the window was empty (no audio had been captured during
    // playback). The assistant then reported "no fresh audio captured yet" even
    // though the user had been playing for well over 6 s. Eager allocation makes
    // capture work from the very first audio block, so on-demand inference
    // always has a full window ready. The ~4 MiB cost (~16 MiB at 192 kHz) is
    // consistent with the other always-allocated model buffers below and is a
    // reasonable trade for a working on-demand path. ensureRing() remains as a
    // defensive idempotent allocator for tests that skip prepare().
    //
    // Allocate inline (not via ensureRing()) because ensureRing() gates on
    // prepared_, which isn't set until the end of prepare(). The ring pointer
    // is published with release semantics AFTER construction completes, so the
    // audio thread's acquire-load in capture() sees either nullptr or a fully
    // constructed AudioCaptureRing — never a torn object (C-3 FIX).
    ringStorage_ = std::make_unique<AudioCaptureRing>(config_.captureRingFrames);
    ring_.store(ringStorage_.get(), std::memory_order_release);

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

		    // AUDIT-FIX (P2, 2026-06-27): size the on-demand scratch buffers alongside
		    // the shared set so requestDecisionNow() can run without holding inferMutex_
		    // across the resample/normalize stages. Sized identically to the shared set.
		    // M-3 AUDIT-NOTE: The duplication of these buffers (shared + onDemand) is
		    // intentional and necessary for concurrency. requestDecisionNow() runs on
		    // the MCP thread while runCycle() runs on the analysis thread; merging them
		    // would require a mutex across the expensive resample stage, deserializing
		    // inference requests. The ~160 KB cost (5 buffers × ~32 KB each) is
		    // acceptable for a thread-safety guarantee.
		    onDemandL_.assign(hostFrames, 0.0f);
	    onDemandR_.assign(hostFrames, 0.0f);
	    onDemandModelL_.assign(kSonicMasterSegmentFrames, 0.0f);
	    onDemandModelR_.assign(kSonicMasterSegmentFrames, 0.0f);
	    onDemandInterleaved_.assign(2 * kSonicMasterSegmentFrames, 0.0f);
	    onDemandDecision_.fill(0.0f);

    consecutiveFailures_.store(0, std::memory_order_relaxed);
    lastPlanId_ = 0;
    nextPlanId_ = 1;
    // AUDIT-FIX (P4 regression): a fresh prepare is the initial-fill state —
    // the first capture sequence must NOT arm the transition guard.
    capturedSinceLastFlush_.store(0, std::memory_order_relaxed);
    everCapturedFullWindow_.store(false, std::memory_order_relaxed);
    // DIAGNOSTIC (2026-06-26): clear any rejection detail from a prior session so a
    // stale snapshot can't be misread after re-prepare. Safe to touch here — the
    // analysis thread is joined (release() was called at the top of prepare).
    clearSafetyRejection_();
    // Stage D: reset the closed LUFS loop so a fresh prepare starts open-loop.
    // AUDIT-FIX (L2-1): relaxed stores — safe after the analysis thread has joined.
    feedbackActive_.store(false, std::memory_order_relaxed);
    feedbackTargetLufs_.store(-14.0f, std::memory_order_relaxed);
    lastAppliedTargetLufs_.store(-14.0f, std::memory_order_relaxed);
    lastMeasuredLufs_.store(0.0f, std::memory_order_relaxed);
    lastLufsError_.store(0.0f, std::memory_order_relaxed);
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
	    // CAPTURE-DECOUPLE (2026-06-26): capture is NO LONGER gated by active_.
	    // Previously capture() bailed when the SonicMaster preview toggle was off
	    // (its default), so the ring never filled and the on-demand path
	    // (requestDecisionNow, called by sonicmaster_decision / mastering.neural_apply)
	    // always failed with "not enough audio captured yet" — even though the model
	    // itself was loaded (isAvailable() == true). The assistant then surfaced this
	    // as "Inference failed or model unavailable" and the LLM hallucinated an
	    // inference-server story. active_ now gates ONLY the background auto-apply
	    // cycle; capture runs whenever the engine is prepared so on-demand inference
	    // works the moment the assistant calls it. write() is a tight lock-free
	    // interleaved memcpy (~2 stores/frame, no allocation) — negligible cost.
	    if (!prepared_.load(std::memory_order_acquire)
	        || left == nullptr || right == nullptr)
	        return;
	    // C-3 FIX (audit): acquire-load the published ring pointer. Paired with
	    // the release-store in ensureRing()/prepare()/release(), this guarantees
	    // we see either a fully-constructed AudioCaptureRing or nullptr — never a
	    // torn or partially-constructed object.
	    auto* ring = ring_.load(std::memory_order_acquire);
	    if (ring == nullptr)
	        return;

	    // AUDIT-FIX (P4, 2026-06-27): first-block-after-silence / source-change
	    // detection. If the ring was flushed (by flushCaptureRing or
	    // notifyAudioSourceChanged) and this is the first write since the flush,
	    // arm the transition guard so the next analysis cycle discards the blending
	    // window. This is a best-effort guard: block-based monitoring means a
	    // source change mid-block won't be caught here, but the explicit
	    // notifyAudioSourceChanged path (called from PluginProcessor::reset) plus
	    // the paramChangePending_ check in runCycle cover the common cases.
	    //
	    // AUDIT-FIX (P4 regression): the original implementation armed the guard
	    // whenever capturedSinceLastFlush_ == 0, which includes the INITIAL fill
	    // right after prepare(). That made the first analysis cycle always discard
	    // its window (InsufficientAudio) — a hard failure for one-shot tests and
	    // a one-cycle latency hit on every fresh playback start. Gate the arming
	    // on everCapturedFullWindow_ so only a genuine mid-session flush (after we
	    // have already built a window) arms the guard.
	    if (everCapturedFullWindow_.load(std::memory_order_relaxed)
	        && capturedSinceLastFlush_.load(std::memory_order_relaxed) == 0u && n > 0)
	    {
	        // Ring was empty (just flushed mid-session). Arm the transition guard so
	        // the analysis thread discards the next capture window.
	        paramChangePending_.store(true, std::memory_order_relaxed);
	        paramChangeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
	            std::chrono::steady_clock::now().time_since_epoch()).count();
	    }

	    ring->write(left, right, n);
	    const std::size_t prev = capturedSinceLastFlush_.load(std::memory_order_relaxed);
	    capturedSinceLastFlush_.store(prev + n, std::memory_order_relaxed);
	    // Once we've captured at least one full analysis window worth of audio,
	    // a subsequent flush counts as a genuine source change (not the initial fill).
	    if (!everCapturedFullWindow_.load(std::memory_order_relaxed)
	        && (prev + n) >= captureL_.size())
	    {
	        everCapturedFullWindow_.store(true, std::memory_order_relaxed);
	    }
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
	    // AUDIT-FIX (P4, 2026-06-27): reset the first-block-after-silence counter
	    // so the next capture() call arms the transition guard. The counter is a
	    // relaxed atomic — safe from the message thread.
	    capturedSinceLastFlush_.store(0u, std::memory_order_relaxed);
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

GenreEqPrior SonicMasterAnalysisEngine::buildEqPrior_() noexcept
{
    GenreEqPrior prior {};  // defaults: no curve, no measurement, blend 0
    const int curveIdx = genreCurveIdx_.load(std::memory_order_relaxed);
    const float blend = residualBlend_.load(std::memory_order_relaxed);
    if (curveIdx < 0 || blend <= 0.0f || applicationEngine_ == nullptr)
        return prior;  // opt-out: no curve selected, blend disabled, or no analyzer

    // Refresh the measured 8-band tonal balance from the live spectrum snapshot.
    RealtimeSpectrumAnalyzer::SpectrumSnapshot snap;
    if (!applicationEngine_->getSpectrumAnalyzer().getSnapshot(snap) || snap.frameIndex == 0)
        return prior;  // no valid spectrum yet
    genreMeasuredBands_ = extractTonalBalanceDb(snap);

    if (curveIdx >= 0 && curveIdx < static_cast<int>(kMasteringTargetCurves.size()))
    {
        prior.curve = &kMasteringTargetCurves[static_cast<std::size_t>(curveIdx)];
        prior.measuredBandDb = genreMeasuredBands_.data();
        prior.residualBlend = std::clamp(blend, 0.0f, 1.0f);
    }
    return prior;
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
        // AUDIT-F1.5: surface InsufficientAudio (symmetric with requestDecisionNow)
        // so the assistant can tell a background-cycle skip from a real failure.
        lastCycleFailure_.store(static_cast<std::uint8_t>(DecisionFailure::InsufficientAudio),
                                std::memory_order_release);
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
        const std::uint64_t changeNs = paramChangeNs_.load(std::memory_order_relaxed);  // AUDIT-FIX (L1-4): was plain read
        const double windowSeconds = static_cast<double>(hostFrames) / sampleRate_;
        const std::uint64_t windowNs = static_cast<std::uint64_t>(windowSeconds * 1e9);
        const std::uint64_t settleNs = static_cast<std::uint64_t>(paramSettleSeconds_.load(std::memory_order_relaxed) * 1e9);
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
            // AUDIT-F1.5: surface the transition-guard skip so the assistant
            // understands the cycle deliberately re-collected (not a failure).
            lastCycleFailure_.store(static_cast<std::uint8_t>(DecisionFailure::InsufficientAudio),
                                    std::memory_order_release);
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
    {
        // AUDIT-F1.5: surface SilentInput (symmetric with requestDecisionNow's
        // identical gate at the on-demand path) so the assistant gets the same
        // reason on a background-cycle skip — was a bare false before this fix.
        lastCycleFailure_.store(static_cast<std::uint8_t>(DecisionFailure::SilentInput),
                                std::memory_order_release);
        return false;
    }
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
        // AUDIT-F1.5: surface InferenceRejected (symmetric with requestDecisionNow).
        lastCycleFailure_.store(static_cast<std::uint8_t>(DecisionFailure::InferenceRejected),
                                std::memory_order_release);
        return false;
    }
    consecutiveFailures_.store(0, std::memory_order_relaxed);

    ValidatedNeuralMasteringPlan plan {};
    // Stage 1 (genre prior) + Stage D (closed LUFS loop). Precedence on the
    // background path: closed-loop feedback (highest, it has measured evidence)
    // → genre prior → model default (lowest). The closed loop only engages after
    // the first apply; until then the genre prior shapes the recommendation. The
    // decode-side hook (Stage A) honors whichever finite value wins here.
    const float genrePrior = genreTargetLufs_.load(std::memory_order_relaxed);
    // AUDIT-FIX (L2-1): acquire loads for closed-loop state read on analysis thread.
    // (These are also written by this thread, so relaxed would suffice for self-reads,
    //  but acquire is correct and consistent with the getClosedLoopState pattern.)
    const bool fbActive = feedbackActive_.load(std::memory_order_acquire);
    const float decodeTargetLufs = fbActive
        ? feedbackTargetLufs_.load(std::memory_order_acquire)
        : (std::isfinite(genrePrior) ? genrePrior : more_phi::kUseModelTargetLufs);
    if (!decodeSonicMasterDecision(decision_.data(), decision_.size(), sampleRate_, plan,
                                   decodeTargetLufs, buildEqPrior_()))
    {
        // AUDIT-F1.5: surface DecodeFailed (symmetric with requestDecisionNow).
        lastCycleFailure_.store(static_cast<std::uint8_t>(DecisionFailure::DecodeFailed),
                                std::memory_order_release);
        return false;
    }
    plan.sourcePlanId = nextPlanId_++;
    plan.capturedAtSteadyClockNs = captureTimeNs_;  // stamp capture instant for staleness guard
    // Record the target this plan actually applied (pre-safety-projection value,
    // for the error calc). Inverse of the decoder map: lufs = -14 + value*6.
    lastAppliedTargetLufs_.store(std::clamp(-14.0f + plan.projectedTargets.loudness[0] * 6.0f,
                                        kFeedbackMinTargetLu, kFeedbackMaxTargetLu),
                                  std::memory_order_release);

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
        // DIAGNOSTIC (2026-06-26): record the specific issue(s) on the background
        // path too, so an on-demand sonicmaster_decision call immediately afterward
        // can report why the last background cycle held (e.g. low_confidence). The
        // on-demand path uses its own decisionPolicy copy but reads the same
        // lastSafetyRejection_ snapshot.
        recordSafetyRejection_(verdict, candidate.confidence);
        // AUDIT-F1.5: surface SafetyRejected (symmetric with requestDecisionNow).
        lastCycleFailure_.store(static_cast<std::uint8_t>(DecisionFailure::SafetyRejected),
                                std::memory_order_release);
        status_.store(Status::HeldLowConfidence, std::memory_order_release);
        return false;
    }
    clearSafetyRejection_();
    // AUDIT-F1.5: a successful cycle clears the structured skip reason.
    lastCycleFailure_.store(static_cast<std::uint8_t>(DecisionFailure::None),
                            std::memory_order_release);

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
	    const bool internalActive = (applicationEngine_ != nullptr)
	        && applicationEngine_->isActive()
	        && applicationEngine_->isIntelligenceActive();
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
        // AUDIT-FIX (F3.2, 2026-06-27): guard on FINITENESS, not just m.valid. The
        // snapshot's valid flag means "engine attached" — but LUFSMeter /
        // TruePeakEstimator initialse to -infinity and only go finite once signal
        // crosses the gates. Before this guard, when the engine was attached but
        // no signal had crossed the gate, m.lufsIntegrated was -inf, so
        // lastLufsError_ = target - (-inf) = +inf, which serialised as `null`
        // via sonicmasterClosedLoopJson() and could spuriously drive the nudge
        // clamp to its +/- 1.0 LU bound every cycle. Now a non-finite loudness
        // reading holds the loop at its last good target and reports a neutral
        // (zero) error instead of a misleading +inf.
        if (m.valid && std::isfinite(m.lufsIntegrated))
        {
            // AUDIT-FIX (L2-1): release stores for closed-loop state written on
            // analysis thread, paired with acquire loads in getClosedLoopState().
            lastMeasuredLufs_.store(m.lufsIntegrated, std::memory_order_release);
            const float appliedTarget = lastAppliedTargetLufs_.load(std::memory_order_acquire);
            const float lufsError = appliedTarget - m.lufsIntegrated; // + = too quiet
            lastLufsError_.store(lufsError, std::memory_order_release);
            if (std::abs(lufsError) > kFeedbackDeadbandLu)
            {
                // Nudge the next target toward reducing the error. Sign: if signal
                // is too quiet (error > 0), raise the target; if too loud, lower it.
                const float nudge = std::clamp(lufsError,
                                               -kFeedbackMaxCorrectionLu,
                                               kFeedbackMaxCorrectionLu);
                feedbackTargetLufs_.store(std::clamp(appliedTarget + nudge,
                                                 kFeedbackMinTargetLu,
                                                 kFeedbackMaxTargetLu),
                                          std::memory_order_release);
                feedbackActive_.store(true, std::memory_order_release);
            }
            else
            {
                // On-target: lock the loop at the current target (no drift).
                feedbackTargetLufs_.store(appliedTarget, std::memory_order_release);
                feedbackActive_.store(true, std::memory_order_release);
            }
        }
        else
        {
            // Measurement absent or non-finite (engine attached but no signal has
            // crossed the LUFS gate yet): report a NEUTRAL error so the closed-loop
            // JSON never emits a misleading +inf, and leave the loop holding its
            // last good target rather than guessing.
            lastLufsError_.store(0.0f, std::memory_order_release);
        }
        // If measurement is invalid (not enough signal), leave feedbackTargetLufs_
        // unchanged — the loop holds its last good target rather than guessing.
    }

    return true;
}

bool SonicMasterAnalysisEngine::requestDecisionNow(float targetLufs,
	                                                    ValidatedNeuralMasteringPlan& outPlan,
	                                                    float* outRawDecision,
	                                                    std::size_t outRawCapacity,
	                                                    DecisionFailure* outFailure) noexcept
{
	    // On-demand path for the AI assistant's sonicmaster_decision MCP tool.
	    // Mirrors runCycle() but: (a) takes a target-LUFS argument, (b) does NOT
	    // require active_ to be set, (c) does NOT apply the plan or touch status_,
	    // (d) returns the decoded plan + raw decision for the caller to act on.
	    //
	    // AUDIT-FIX (P2, 2026-06-27): uses DEDICATED on-demand scratch buffers
	    // (onDemandL_/R_/ModelL_/ModelR_/Interleaved_/Decision_) so the
	    // resample/normalize stages run WITHOUT inferMutex_. Only the
	    // source_->infer() call and the decode/safety block need the mutex
	    // (the ONNX session is not thread-safe). This reduces the critical section
	    // from ~500ms to ~200ms and prevents the background cycle from blocking
	    // on-demand MCP tools during resampling.
	    //
	    // DIAGNOSTIC (2026-06-26): every failure return now stamps *outFailure with
	    // the structured reason, so the MCP tool can surface the ACTUAL gate that
	    // tripped instead of the opaque "Inference failed or model unavailable" that
	    // forced the assistant to confabulate causes. See DecisionFailure enum.
	    if (outFailure != nullptr) *outFailure = DecisionFailure::None;

	    ensureRing();  // PERF-MEM: lazy ring allocation
	    // C-3 FIX (audit): acquire-load the atomic ring pointer.
	    auto* ring = ring_.load(std::memory_order_acquire);
	    if (ring == nullptr || source_ == nullptr || !isAvailable())
	    {
	        if (outFailure != nullptr) *outFailure = DecisionFailure::NotPrepared;
	        return false;
	    }

	    // ── Stage 1: drain ring into ON-DEMAND scratch buffers (no mutex) ─────
	    // This is safe because onDemand* buffers are exclusively owned by this
	    // function (allocated in prepare(), only touched here). The ring is
	    // atomic-published (C-3 fix) — acquire-load safe from any thread.
	    const std::size_t hostFrames = onDemandL_.size();
	    const std::size_t got = ring->readNewest(hostFrames, onDemandL_.data(), onDemandR_.data());
	    if (got < hostFrames)
	    {
	        if (outFailure != nullptr) *outFailure = DecisionFailure::InsufficientAudio;
	        return false;  // not enough audio captured yet
	    }

	    // Stamp capture time for the staleness guard.
	    captureTimeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
	        std::chrono::steady_clock::now().time_since_epoch()).count();

	    // ── Stage 2: resample host-rate → 44.1k (no mutex) ───────────────────
	    if (std::abs(sampleRate_ - 44100.0) < 0.5)
	    {
	        std::copy_n(onDemandL_.data(), kSonicMasterSegmentFrames, onDemandModelL_.data());
	        std::copy_n(onDemandR_.data(), kSonicMasterSegmentFrames, onDemandModelR_.data());
	    }
	    else
	    {
	        resamplePolyphase(onDemandL_.data(), hostFrames, kSonicMasterSegmentFrames, onDemandModelL_.data());
	        resamplePolyphase(onDemandR_.data(), hostFrames, kSonicMasterSegmentFrames, onDemandModelR_.data());
	    }

	    // ── Stage 3: peak-normalize (no mutex) ──────────────────────────────
	    const float peak = std::max(peakAbs(onDemandModelL_.data(), kSonicMasterSegmentFrames),
	                                peakAbs(onDemandModelR_.data(), kSonicMasterSegmentFrames));
	    if (!std::isfinite(peak) || peak < 1e-15f)
	    {
	        if (outFailure != nullptr) *outFailure = DecisionFailure::SilentInput;
	        return false;
	    }
	    const float gain = kSonicMasterPeakTargetLinear / peak;
	    for (std::size_t i = 0; i < kSonicMasterSegmentFrames; ++i)
	    {
	        onDemandInterleaved_[2 * i + 0] = onDemandModelL_[i] * gain;
	        onDemandInterleaved_[2 * i + 1] = onDemandModelR_[i] * gain;
	    }

	    // ── Stage 4: infer + decode + validate (under inferMutex_) ──────────
	    // The ONNX session (source_) is NOT thread-safe — only the analysis
	    // thread or this function may call infer() at any moment. The mutex
	    // serializes access.
	    {
	        std::lock_guard<std::mutex> inferLock(inferMutex_);

	        // Propagate the requested target LUFS to the inference source.
	        source_->setTargetLufs(targetLufs);

	        if (!source_->infer(onDemandInterleaved_.data(), onDemandDecision_.data(),
	                            onDemandDecision_.size()))
	        {
	            if (outFailure != nullptr) *outFailure = DecisionFailure::InferenceRejected;
	            return false;
	        }

	        if (outRawDecision != nullptr && outRawCapacity >= kSonicMasterDecisionWidth)
	            std::copy_n(onDemandDecision_.data(), kSonicMasterDecisionWidth, outRawDecision);

	        ValidatedNeuralMasteringPlan plan {};
	        // Stage A: honor caller's explicit target_lufs at decode time.
	        float onDemandTarget = targetLufs;
	        if (!std::isfinite(onDemandTarget))
	        {
	            const float genrePrior = genreTargetLufs_.load(std::memory_order_relaxed);
	            if (std::isfinite(genrePrior))
	                onDemandTarget = genrePrior;
	        }
	        if (!decodeSonicMasterDecision(onDemandDecision_.data(), onDemandDecision_.size(),
	                                       sampleRate_, plan,
	                                       std::isfinite(onDemandTarget) ? onDemandTarget : kUseModelTargetLufs,
	                                       buildEqPrior_()))
	        {
	            if (outFailure != nullptr) *outFailure = DecisionFailure::DecodeFailed;
	            return false;
	        }
	        plan.sourcePlanId = nextPlanId_++;
	        plan.capturedAtSteadyClockNs = captureTimeNs_;

	        NeuralMasteringRuntimeState runtime {};
	        runtime.sampleRate = sampleRate_;
	        runtime.channelCount = 2;
	        runtime.layout = NeuralMasteringLayout::Stereo;
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
	        {
	            recordSafetyRejection_(verdict, candidate.confidence);
	            if (outFailure != nullptr) *outFailure = DecisionFailure::SafetyRejected;
	            return false;
	        }

	        clearSafetyRejection_();
	        outPlan = verdict.plan;
	    } // inferMutex_ released

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
