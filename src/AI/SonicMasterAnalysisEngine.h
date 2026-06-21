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
    virtual bool infer(const float* stereoInterleaved, float* outDecision,
                       std::size_t outCapacity) noexcept = 0;
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
    void setActive(bool active) noexcept { active_.store(active, std::memory_order_relaxed); }
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

private:
    void analysisLoop() noexcept;
    bool runCycle() noexcept; // returns true if a plan was applied
    void applyRamped(const ValidatedNeuralMasteringPlan& plan) noexcept;

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
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> active_ { false };
    std::atomic<bool> stopRequested_ { false };
    std::atomic<bool> prepared_ { false };
    std::atomic<Status> status_ { Status::Disabled };
    std::atomic<int> consecutiveFailures_ { 0 };
    std::uint64_t lastPlanId_ = 0;
    std::uint64_t nextPlanId_ = 1;
    double sampleRate_ = 48000.0;
};

} // namespace more_phi
