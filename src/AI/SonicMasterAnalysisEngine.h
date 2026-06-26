/*
 * More-Phi — AI/SonicMasterAnalysisEngine.h
 *
 * Orchestrator for the realtime neural mastering path. Owns the capture ring,
 * the analysis thread, the (injected) inference source, the safety policy, and
 * the message-thread parameter handoff.
 *
 * Thread model (three domains, strict):
 *   Audio thread    — capture() only. Lock-free ring write + relaxed atomic.
 *   Analysis thread — runs the cycle: drain ring -> resample -> normalize ->
 *                     infer -> decode -> safety-validate -> callAsync handoff.
 *                     Owns the ONNX session exclusively (created in prepare(),
 *                     destroyed in release() AFTER the thread is joined).
 *   Message thread  — applyRamped(): updates AutoMasteringEngine bookkeeping
 *                     and ramps DSP parameters via the existing applyValidated
 *                     Plan() surface.
 *
 * The engine is NOT an INeuralMasteringModelRunner — it sits parallel to the
 * 63->72 NeuralMasteringController path, both feeding AutoMasteringEngine.
 *
 * Lifecycle: prepare() starts the analysis thread idle; setActive(true) starts
 * capturing + cycling; release() joins the thread before any owned state is
 * destroyed. The engine may be re-prepared after release().
 */
#pragma once

#include "AI/SonicMasterDecisionDecoder.h"
#include "AI/SonicMasterDecisionRunner.h"
#include "Core/AudioCaptureRing.h"
#include "Core/NeuralMasteringSafetyPolicy.h"
#include "Core/NeuralMasteringTypes.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace more_phi {

class AutoMasteringEngine;

// Abstracts "run inference on a waveform" so tests can substitute a stub.
// Real use wires SonicMasterRunnerInferenceSource; tests pass a fake that
// returns a canned decision vector without any ONNX.
class ISonicMasterInferenceSource
{
public:
    virtual ~ISonicMasterInferenceSource() = default;
    [[nodiscard]] virtual bool isAvailable() const noexcept = 0;
    // Refresh any cached availability from a background thread. Default no-op;
    // sources whose isAvailable() reads a cache should override this and do the
    // (possibly blocking) work here, never on the message thread.
    virtual void refreshProbe() noexcept {}
    virtual bool infer(const float* stereoInterleaved, float* outDecision,
                       std::size_t outCapacity) noexcept = 0;
    // Set the mastering target LUFS the next infer() will request. The HTTP
    // source sends it as a query param; the ONNX source ignores it — the ONNX
    // graph was exported with input count == 1 (waveform only) and the target
    // loudness is baked into the exported computation. Without the HTTP path's
    // explicit target, the decoded loudness goal is unconstrained by the caller.
    // Default impl is a no-op so existing stubs/test sources don't need to
    // override it.
    virtual void setTargetLufs(float /*lufs*/) noexcept {}
    // DIAG (2026-06-26): the last error message from infer() (empty if the last
    // run succeeded). Lets the MCP failure response report the real ORT error
    // instead of swallowing it. Default returns empty (stubs/tests).
    [[nodiscard]] virtual std::string lastInferenceError() const noexcept { return {}; }
    [[nodiscard]] virtual std::uint64_t inferenceRunCount() const noexcept { return 0; }
    [[nodiscard]] virtual std::uint64_t inferenceFailCount() const noexcept { return 0; }
};

// Adapter that turns SonicMasterDecisionRunner into an ISonicMasterInferenceSource.
class SonicMasterRunnerInferenceSource final : public ISonicMasterInferenceSource
{
public:
    explicit SonicMasterRunnerInferenceSource(SonicMasterDecisionRunner& runner) noexcept
        : runner_(runner) {}
    [[nodiscard]] bool isAvailable() const noexcept override { return runner_.isAvailable(); }
    bool infer(const float* stereoInterleaved, float* outDecision,
               std::size_t outCapacity) noexcept override
    { return runner_.runDecision(stereoInterleaved, outDecision, outCapacity); }
    [[nodiscard]] std::string lastInferenceError() const noexcept override
    { return runner_.getLastRunError(); }
    [[nodiscard]] std::uint64_t inferenceRunCount() const noexcept override
    { return runner_.getRunCount(); }
    [[nodiscard]] std::uint64_t inferenceFailCount() const noexcept override
    { return runner_.getFailCount(); }
private:
    SonicMasterDecisionRunner& runner_;
};

// AUDIT-FIX-1: genuine classical measurements pulled from AutoMasteringEngine's
// already-running BS.1770-4 LUFS meter, true-peak estimator, spectrum analyzer,
// and stereo-field analyzer. These are MEASUREMENTS (traceable to ITU-R
// BS.1770-4 / EBU R128), to be reported alongside the model ESTIMATE in
// requestDecisionNow's output so the assistant can never again present the
// peak-normalized model target as a measurement of the input.
struct SonicMasterMeasurementSnapshot
{
    float lufsIntegrated  = 0.0f;
    float lufsShortTerm   = 0.0f;
    float lufsMomentary   = 0.0f;
    float lra             = 0.0f;
    float truePeakDbtp    = 0.0f;
    float spectralCentroidHz = 0.0f;
    float spectralTilt    = 0.0f;
    float stereoWidth     = 0.0f;   // 0..1
    float correlationMid  = 0.0f;   // -1..+1
    // Metrics/AUDIT: blind-spot metrics now computed by RealtimeSpectrumAnalyzer.
    float thdPercent          = 0.0f;   // THD H2..H5 / fundamental, %
    float crestFactorProgram  = 0.0f;   // program-level peak/RMS (smoothed)
    bool  valid           = false;
};

struct SonicMasterAnalysisEngineConfig
{
    double analysisIntervalSeconds = 3.0;
    float  confidenceFloor         = kSonicMasterDefaultConfidence;
    int    consecutiveFailureLimit = 3;
    // 8 s of capture, rate-proportional. prepare() sets this to round(8.0 * sampleRate)
    // (clamped to [2s@44.1k, 32s@192k]); AudioCaptureRing then pow2-rounds it. At
    // 48 kHz → ~4.0 MiB; at 192 kHz → ~16.0 MiB. The default here is only used if
    // prepare() is never called (tests); production always re-sizes it.
    std::size_t captureRingFrames = 8u * 48000u;
};

// DIAGNOSTIC (2026-06-26): structured failure reason for requestDecisionNow. The
// on-demand path (sonicmaster_decision / mastering.neural_apply) previously got a
// bare bool and the MCP handler collapsed every false into one opaque string,
// forcing the assistant to confabulate a cause ("no audio captured", "inference
// server down"). This enum lets the tool report the ACTUAL gate that tripped so
// the assistant can relay a truthful, actionable reason to the user. Order mirrors
// the order the gates are checked in requestDecisionNow.
enum class DecisionFailure : std::uint8_t
{
    None,               // success (requestDecisionNow returned true)
    NotPrepared,        // ring is null, source is null, or !isAvailable()
    InsufficientAudio,  // ring captured < hostFrames (the ~6s window isn't full yet)
    SilentInput,        // captured peak < 1e-15f (silence or all-NaN); model input undefined
    InferenceRejected,  // source_->infer() returned false (ONNX Run threw or returned no output)
    DecodeFailed,       // decodeSonicMasterDecision could not parse the model output
    SafetyRejected      // NeuralMasteringSafetyPolicy verdict was not accepted
};

class SonicMasterAnalysisEngine
{
public:
    SonicMasterAnalysisEngine();
    ~SonicMasterAnalysisEngine();

    SonicMasterAnalysisEngine(const SonicMasterAnalysisEngine&) = delete;
    SonicMasterAnalysisEngine& operator=(const SonicMasterAnalysisEngine&) = delete;

    // Inject the inference source (real runner or test stub). Caller owns it;
    // must outlive this engine. Passing nullptr reverts to "unavailable".
    void setInferenceSource(ISonicMasterInferenceSource* source) noexcept;
    // AUDIT LOW-4: set a secondary inference source that the engine automatically
    // swaps to when the primary fails consecutiveFailureLimit times. The fallback
    // is tried once; if it also fails, the engine disables as before. Pass nullptr
    // to clear. Caller owns the source; must outlive this engine.
    void setFallbackInferenceSource(ISonicMasterInferenceSource* source) noexcept { fallbackSource_ = source; }
    void setApplicationEngine(AutoMasteringEngine* engine) noexcept { applicationEngine_ = engine; }

    // Message thread. Sizes the capture ring + scratch buffers and starts the
    // analysis thread idle. Safe to re-call (joins any prior thread first).
    void prepare(double sampleRate, int maxBlockSize);

    // Message thread. Signals the analysis thread to stop, joins it, then drops
    // the ring/scratch. No-op if not prepared.
    void release() noexcept;

    // Audio thread: copy a stereo block into the capture ring. noexcept, no
    // locks, no allocation. Early-returns when inactive or unprepared.
    void capture(const float* left, const float* right, std::size_t n) noexcept;

    // Message thread: reset the capture ring, discarding all previously captured
    // audio. Call this when the hosted plugin or audio source changes to prevent
    // inference from mixing old and new content. Safe to call while the engine is
    // active — the analysis thread will see an empty ring on its next cycle.
    void flushCaptureRing() noexcept;

    // P2.8 (AUDIT): transition guard. Call from the hosted-plugin parameter write
    // path (AI/MCP/morph edits) when a hosted plugin parameter changes. Records the
    // instant of the change so runCycle() can detect that the current capture window
    // straddles a parameter transition — a window spanning a param change is a hybrid
    // of two states and must NOT be analyzed (the model would produce a decision for a
    // state that never existed). The cycle discards such a window, flushes the ring,
    // and reports CollectingAudio until a clean window accumulates.
    //
    // Thread-safe and cheap: one relaxed atomic flag + one relaxed timestamp store.
    // Safe to call from the message/MCP thread (enqueueParameterSet). NOT intended
    // for the audio-thread drain hot path — wire it at the enqueue point instead.
    void notifyHostedParameterChanged() noexcept
    {
        paramChangePending_.store(true, std::memory_order_relaxed);
        paramChangeNs_ = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // P2.8: configurable settling period (seconds). After a parameter change the
    // engine waits this long before trusting a capture window, so the hosted
    // plugin's internal state (filters, lookahead, tails) has flushed. Defaults to
    // 0.5 s; raise for plugins with long tails/reverb. Message thread.
    void setParamTransitionSettleSeconds(double seconds) noexcept
    { paramSettleSeconds_ = seconds > 0.0 ? seconds : 0.0; }

    // Any thread (atomic). When true, capture + cycling run; when false, capture
    // is a no-op and the analysis thread sleeps. DSP params are HELD, not reset.
    // PERF-MEM: First activation lazily allocates the rate-proportional capture
    // ring (~4 MiB at 48 kHz, ~16 MiB at 192 kHz — see SonicMasterAnalysisEngineConfig).
    void setActive(bool active) noexcept
    {
        active_.store(active, std::memory_order_relaxed);
        if (active) ensureRing();
    }
    [[nodiscard]] bool isActive() const noexcept { return active_.load(std::memory_order_relaxed); }

    [[nodiscard]] bool isAvailable() const noexcept;

    // Status surfaced to the UI. Any thread (atomic).
    enum class Status : std::uint8_t
    {
        Disabled,
        CollectingAudio,
        Applied,
        // F2/AUDIT: a plan was decoded, safety-validated, and written into the
        // AutoMasteringEngine's DSP setters — BUT that chain is not active in the
        // audio path (active_==false), so no audio sample flows through it
        // and the hosted plugin is untouched. Distinct from Applied so the UI
        // and the MCP client cannot mistake a dormant apply for an audible one.
        AppliedNoAudioPath,
        // AUDIT-FIX (Fix 2/6): a plan was applied but readback verification found
        // either <80% of the enqueued params actually landed on the hosted plugin,
        // or the applicator only managed to enqueue <80% of the requested slots
        // (partial map / queue contention / ambiguous names). Distinct from Applied
        // so the assistant cannot claim a clean apply when params drifted or were
        // skipped. The full breakdown is available via getLastApplyVerification().
        AppliedPartial,
        HeldLowConfidence,
        ErrorAutoDisabled
    };
    [[nodiscard]] Status getStatus() const noexcept { return status_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t getLastPlanId() const noexcept { return lastPlanId_; }

    // Stage D (2026-06-26): closed LUFS feedback loop readback for the assistant.
    // feedbackActive = the loop has seen at least one apply and is correcting.
    // lastLufsError = target - measured (positive = signal is too quiet vs target).
    // Constants are public (compile-time safety contract, like recallRampBlocks)
    // so tests can pin the deadband + per-cycle cap that prevent runaway/oscillation.
    static constexpr float kFeedbackDeadbandLu      = 0.5f;   // don't correct < 0.5 LU off
    static constexpr float kFeedbackMaxCorrectionLu = 1.0f;   // cap per-cycle nudge
    static constexpr float kFeedbackMinTargetLu     = -23.0f; // engine clamp floor
    static constexpr float kFeedbackMaxTargetLu     = -8.0f;  // engine clamp ceil
    struct ClosedLoopState
    {
        bool  feedbackActive = false;
        float lastAppliedTargetLufs = -14.0f;
        float lastMeasuredLufs = 0.0f;
        float lastLufsError = 0.0f;
        float nextTargetLufs = -14.0f;
    };
    [[nodiscard]] ClosedLoopState getClosedLoopState() const noexcept
    {
        return { feedbackActive_, lastAppliedTargetLufs_, lastMeasuredLufs_,
                 lastLufsError_, feedbackActive_ ? feedbackTargetLufs_ : -14.0f };
    }
    // F2/AUDIT: true when the last apply actually reached an active audio path.
    // Mirrors the Applied vs AppliedNoAudioPath split for clients that read a
    // boolean rather than the enum.
    [[nodiscard]] bool lastApplyReachedAudioPath() const noexcept { return lastApplyReachedAudioPath_.load(std::memory_order_acquire); }

    // CAPTURE-TELEMETRY (2026-06-26): live capture-ring state for diagnostics.
    // Exposed so sonicmaster_decision can report WHY inference was skipped —
    // "no audio captured" vs "ring not allocated" vs "prepared=false" — instead
    // of the generic catch-all that left the assistant guessing. All reads are
    // atomic acquire-loads (safe from any thread); the values are advisory and
    // may change between read and use.
    struct CaptureDiagnostics
    {
        bool          prepared       = false;  // prepare() called, release() not
        bool          active         = false;  // preview toggle (gates background apply, NOT capture)
        bool          ringAllocated  = false;  // the AudioCaptureRing exists
        std::uint64_t capturedFrames = 0;      // frames currently in the ring (0..capacity)
        std::size_t   requiredFrames = 0;      // host-rate window the engine needs (captureL_.size())
        std::size_t   ringCapacity   = 0;      // ring capacity in frames
    };
    [[nodiscard]] CaptureDiagnostics getCaptureDiagnostics() const noexcept;

    // DIAG (2026-06-26): pull the last inference error through the source
    // interface so sonicmaster_decision can report the real ORT exception.
    [[nodiscard]] std::string lastInferenceError() const noexcept
    {
        return source_ != nullptr ? source_->lastInferenceError() : std::string{};
    }
    [[nodiscard]] std::uint64_t inferenceRunCount() const noexcept
    {
        return source_ != nullptr ? source_->inferenceRunCount() : 0;
    }
    [[nodiscard]] std::uint64_t inferenceFailCount() const noexcept
    {
        return source_ != nullptr ? source_->inferenceFailCount() : 0;
    }

    // Test hook: run one analysis cycle synchronously on the calling thread
    // (does NOT spawn the background thread). Used by the unit tests to avoid
    // real thread timing. Forces active_=true for the call.
    bool runOneCycleForTest() noexcept;

    /// On-demand mastering decision for the AI assistant (MCP tool
    /// sonicmaster_decision). Synchronous: drains the capture ring, resamples,
    /// runs inference, and returns the decoded plan + raw 44-float decision.
    /// Does NOT apply the plan — the caller (the assistant) decides whether to
    /// apply it via applyValidatedPlan. Returns false when the inference source
    /// is unavailable or there isn't enough captured audio yet (the assistant
    /// should then tell the user to play audio for ~6 s first).
    ///
    /// `targetLufs` is the mastering target fed to the model (default -14).
    /// On success, `outPlan` is the decoded, safety-clamped plan and
    /// `outRawDecision` is the model's raw 44-float vector (for telemetry).
    ///
    /// `outFailure` (DIAGNOSTIC, 2026-06-26): optional out-param receiving the
    /// structured reason the call returned false (DecisionFailure::None on
    /// success). Callers that surface the result to a user/LLM should pass this
    /// so they can report the ACTUAL gate that tripped instead of guessing.
    /// Defaults to nullptr for source compatibility with existing call sites.
    bool requestDecisionNow(float targetLufs,
                            ValidatedNeuralMasteringPlan& outPlan,
                            float* outRawDecision,
                            std::size_t outRawCapacity,
                            DecisionFailure* outFailure = nullptr) noexcept;

    /// AUDIT-FIX-1: genuine classical measurements of the live input, pulled
    /// from AutoMasteringEngine's already-running BS.1770-4 / true-peak /
    /// spectrum / stereo meters. These are MEASUREMENTS, to be reported
    /// alongside the model estimate so the assistant never confuses the two.
    /// Returns valid=false when no application engine is attached.
    SonicMasterMeasurementSnapshot getLiveMeasurements() const noexcept;

    // ── Pending-plan handoff (AUDIT-FIX-R5) ───────────────────────────────
    // Replaces MessageManager::callAsync which can silently drop in headless
    // hosts. The analysis thread stores a validated plan and sets a flag, then
    // optionally invokes a callback (set by the processor) to request message-
    // thread maintenance. The processor's timer polls hasPendingApplication()
    // and calls processPendingApplication() to apply the plan.
    //
    // Set this callback once after construction to wire the engine to the
    // processor's maintenance timer (see morphPositionNotifyPending_ pattern).
    void setMaintenanceRequestCallback(std::function<void()> cb) noexcept
    {
        maintenanceRequestCb_ = std::move(cb);
    }
    [[nodiscard]] bool hasPendingApplication() const noexcept
    {
        return pendingApplication_.load(std::memory_order_acquire);
    }
    // Message thread only — applies the stored plan via applyValidatedPlan()
    // and clears the pending flag.
    void processPendingApplication() noexcept;

private:
    void analysisLoop() noexcept;
    bool runCycle() noexcept; // returns true if a plan was applied
    void applyRamped(const ValidatedNeuralMasteringPlan& plan) noexcept;

    // PERF-MEM: Lazily allocates the rate-proportional capture ring on first use.
    // Called from setActive(true), requestDecisionNow, and runOneCycleForTest.
    // Idempotent — no-op if the ring already exists or prepare() hasn't been called.
    void ensureRing() noexcept;

    SonicMasterAnalysisEngineConfig config_ {};
    AutoMasteringEngine* applicationEngine_ = nullptr;
    ISonicMasterInferenceSource* source_ = nullptr;
    ISonicMasterInferenceSource* fallbackSource_ = nullptr;

    // C-3 FIX (audit): ring_ is published through an atomic raw pointer so the
    // audio thread (capture()) observes either a fully-constructed AudioCaptureRing
    // or nullptr — never a partially-constructed object. ringStorage_ owns the
    // lifetime and is touched only on the message/analysis thread under the
    // existing thread-join + host prepare/process mutual-exclusion discipline
    // (capture() is only called from processBlock; JUCE guarantees prepare()/
    // release() are mutually exclusive with processBlock).
    std::unique_ptr<AudioCaptureRing> ringStorage_;
    std::atomic<AudioCaptureRing*> ring_ { nullptr };
    std::vector<float> captureL_, captureR_;   // host-rate window
    std::vector<float> modelL_, modelR_;       // 44.1k window
    std::vector<float> interleaved_;           // 2 * kSonicMasterSegmentFrames
    std::array<float, kSonicMasterDecisionWidth> decision_ {};

    NeuralMasteringSafetyPolicy safetyPolicy_ {};

    std::thread thread_;
    std::mutex mutex_;          // guards the cv wait below
    std::mutex inferMutex_;     // AUDIT-1: serializes runCycle vs requestDecisionNow scratch use
    std::condition_variable cv_;
    std::atomic<bool> active_ { false };
    std::atomic<bool> stopRequested_ { false };
    std::atomic<bool> prepared_ { false };
    std::atomic<Status> status_ { Status::Disabled };
    // F2/AUDIT: set true only when applyRamped targeted an active app engine.
    std::atomic<bool> lastApplyReachedAudioPath_ { false };
    std::atomic<int> consecutiveFailures_ { 0 };
    std::uint64_t lastPlanId_ = 0;
    std::uint64_t nextPlanId_ = 1;
    double sampleRate_ = 48000.0;

    // Stage D (2026-06-26): closed LUFS feedback loop state. After each apply,
    // runCycle measures the achieved LUFS on the captured (post-hosted-plugin)
    // signal, computes the error vs the target that was just applied, and folds a
    // bounded correction into feedbackTargetLufs_ for the NEXT cycle's decode.
    // feedbackActive_ gates it (off until the first apply lands; reset on
    // prepare/release). Bounded by the public kFeedback* constants above.
    bool  feedbackActive_   = false;
    float feedbackTargetLufs_ = -14.0f;   // next cycle's target override
    float lastAppliedTargetLufs_ = -14.0f; // what the last apply targeted (for error)
    float lastMeasuredLufs_      = 0.0f;   // last achieved LUFS readback
    float lastLufsError_         = 0.0f;   // target - measured (positive = too quiet)
    // AUDIT-IX-8: steady-clock ns of the most recent capture window end. Stamped
    // into each plan so applyRamped can drop stale plans (analysisInterval +
    // inference latency past). Written on the analysis thread under inferMutex_,
    // read on the same thread; relaxed atomic only because it is advisory.
    std::uint64_t captureTimeNs_ = 0;

    // AUDIT-FIX-R5: pending-plan handoff replaces MessageManager::callAsync.
    // The analysis thread writes the plan, then sets the flag (release).
    // The message thread reads the flag (acquire), then reads the plan.
    // Protected by the flag ordering — only one analysis thread writes (under
    // inferMutex_ in runCycle) and only one message thread reads/clears.
    std::atomic<bool> pendingApplication_ { false };
    ValidatedNeuralMasteringPlan pendingPlan_ {};
    std::function<void()> maintenanceRequestCb_;  // set by processor; called after flag is set

    // P2.8 (AUDIT): hosted-parameter transition guard. paramChangePending_ is set by
    // notifyHostedParameterChanged() (message/MCP thread) and consumed+cleared by
    // runCycle() (analysis thread). paramChangeNs_ is the steady-clock instant of
    // the most recent change. Relaxed atomics: advisory — a missed/delayed observation
    // only risks analyzing one stale window, never corrupts data.
    std::atomic<bool> paramChangePending_ { false };
    std::uint64_t paramChangeNs_ = 0;
    double paramSettleSeconds_ = 0.5;   // default settling period after a param change
};

} // namespace more_phi
