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
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
    // source sends it as a query param; the ONNX source ignores it (target is
    // baked into the exported graph). Default impl is a no-op so existing
    // stubs/test sources don't need to override it.
    virtual void setTargetLufs(float /*lufs*/) noexcept {}
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
    bool  valid           = false;
};

struct SonicMasterAnalysisEngineConfig
{
    double analysisIntervalSeconds = 3.0;
    double rampDurationSeconds     = 0.2;   // informational; apply is via applyValidatedPlan
    float  confidenceFloor         = kSonicMasterDefaultConfidence;
    int    consecutiveFailureLimit = 3;
    // 8 s @ 192 kHz, rounded up to a power of two by AudioCaptureRing.
    std::size_t captureRingFrames = 8u * 192000u;
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

    // Any thread (atomic). When true, capture + cycling run; when false, capture
    // is a no-op and the analysis thread sleeps. DSP params are HELD, not reset.
    // PERF-MEM: First activation lazily allocates the ~12.3 MB capture ring.
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
        HeldLowConfidence,
        ErrorAutoDisabled
    };
    [[nodiscard]] Status getStatus() const noexcept { return status_.load(std::memory_order_acquire); }
    [[nodiscard]] std::uint64_t getLastPlanId() const noexcept { return lastPlanId_; }

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
    bool requestDecisionNow(float targetLufs,
                            ValidatedNeuralMasteringPlan& outPlan,
                            float* outRawDecision,
                            std::size_t outRawCapacity) noexcept;

    /// AUDIT-FIX-1: genuine classical measurements of the live input, pulled
    /// from AutoMasteringEngine's already-running BS.1770-4 / true-peak /
    /// spectrum / stereo meters. These are MEASUREMENTS, to be reported
    /// alongside the model estimate so the assistant never confuses the two.
    /// Returns valid=false when no application engine is attached.
    SonicMasterMeasurementSnapshot getLiveMeasurements() const noexcept;

private:
    void analysisLoop() noexcept;
    bool runCycle() noexcept; // returns true if a plan was applied
    void applyRamped(const ValidatedNeuralMasteringPlan& plan) noexcept;

    // PERF-MEM: Lazily allocates the capture ring (~12.3 MB) on first use.
    // Called from setActive(true), requestDecisionNow, and runOneCycleForTest.
    // Idempotent — no-op if the ring already exists or prepare() hasn't been called.
    void ensureRing() noexcept;

    SonicMasterAnalysisEngineConfig config_ {};
    AutoMasteringEngine* applicationEngine_ = nullptr;
    ISonicMasterInferenceSource* source_ = nullptr;

    std::unique_ptr<AudioCaptureRing> ring_;
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
    std::atomic<int> consecutiveFailures_ { 0 };
    std::uint64_t lastPlanId_ = 0;
    std::uint64_t nextPlanId_ = 1;
    double sampleRate_ = 48000.0;
    // AUDIT-IX-8: steady-clock ns of the most recent capture window end. Stamped
    // into each plan so applyRamped can drop stale plans (analysisInterval +
    // inference latency past). Written on the analysis thread under inferMutex_,
    // read on the same thread; relaxed atomic only because it is advisory.
    std::uint64_t captureTimeNs_ = 0;
};

} // namespace more_phi
