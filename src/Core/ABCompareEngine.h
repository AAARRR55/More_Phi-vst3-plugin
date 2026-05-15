/*
 * More-Phi — Core/ABCompareEngine.h
 *
 * A/B comparison and parameter rollback engine for the mastering chain.
 *
 * Workflow:
 *   1. captureCheckpoint()  — save current mastering params to SnapshotBank slot 11
 *   2. Apply candidate parameters from GeneticOptimizer or ChainPlanExecutor
 *   3. After 2 s analysis window, compareAndDecide() compares LUFS/LRA/spectral score
 *   4. If candidate is worse on ≥ 2 metrics → rollback() restores slot 11
 *   5. UI exposes commitCandidate() / rollbackCandidate() for manual override
 *
 * Thread safety:
 *   captureCheckpoint() / rollback() — message thread only.
 *   compareAndDecide()              — message thread only (timer-driven).
 *   isComparing()                   — any thread (atomic read).
 */
#pragma once

#include <atomic>
#include <functional>
#include <juce_core/juce_core.h>
#include "LUFSMeter.h"

namespace more_phi {

class ABCompareEngine : public juce::Timer
{
public:
    static constexpr int kReservedSlot = 11;   // SnapshotBank slot reserved for rollback

    struct Metrics {
        float lufsIntegrated = -200.f;
        float lra            =    0.f;
        float spectralScore  =    0.f;   // composite spectral balance score
    };

    /** Called to capture / restore mastering parameters. */
    using CaptureFunc  = std::function<void(int slot)>;
    using RestoreFunc  = std::function<void(int slot)>;

    ABCompareEngine() = default;
    ~ABCompareEngine() override { stopTimer(); }

    // ── Setup ─────────────────────────────────────────────────────────────────

    void setCaptureFunctions(CaptureFunc capture, RestoreFunc restore)
    {
        captureFunc_ = std::move(capture);
        restoreFunc_ = std::move(restore);
    }

    void setLUFSMeter(LUFSMeter& meter) { meter_ = &meter; }

    // ── A/B flow ──────────────────────────────────────────────────────────────

    /**
     * Capture current mastering params to slot 11.
     * Call before applying a candidate chain plan.
     */
    void captureCheckpoint();

    /**
     * Begin the 2-second analysis window.
     * After 2 s, compareAndDecide() is called automatically.
     */
    void startComparison();

    /** Immediately apply candidate (skip comparison). */
    void commitCandidate();

    /** Immediately rollback to checkpoint. */
    void rollbackCandidate();

    [[nodiscard]] bool isComparing() const noexcept { return comparing_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool hasPendingCandidate() const noexcept { return hasPending_.load(std::memory_order_relaxed); }

    /** Set baseline metrics from the pre-candidate state. Call right before captureCheckpoint(). */
    void setBaselineMetrics(const Metrics& m) { baseline_ = m; }

    /** Read current candidate metrics from LUFSMeter and spectral analyser. */
    Metrics readCurrentMetrics() const noexcept;

private:
    void timerCallback() override;
    void compareAndDecide();

    LUFSMeter*   meter_       = nullptr;
    CaptureFunc  captureFunc_;
    RestoreFunc  restoreFunc_;

    Metrics baseline_{};

    std::atomic<bool> comparing_   { false };
    std::atomic<bool> hasPending_  { false };

    static constexpr int kAnalysisMs = 2000;  // 2-second comparison window
};

} // namespace more_phi
