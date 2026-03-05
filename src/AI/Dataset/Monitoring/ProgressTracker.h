#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <mutex>
#include <string>

namespace morphsnap {

/**
 * ProgressTracker — Thread-safe, real-time progress and ETA tracking.
 *
 * Updated by worker threads after each completed batch.
 * The UI or GenerationLogger can read the latest snapshot at any time.
 */
class ProgressTracker
{
public:
    struct Snapshot
    {
        uint64_t batchesCompleted = 0;
        uint64_t batchesTotal     = 0;
        uint64_t framesCompleted  = 0;
        uint64_t framesTotal      = 0;
        uint64_t validationPasses = 0;
        uint64_t validationFails  = 0;
        double   batchesPerSecond = 0.0;
        double   elapsedSeconds   = 0.0;
        double   etaSeconds       = 0.0;
        float    percentComplete  = 0.0f;
    };

    ProgressTracker() = default;

    /** Reset all counters and restart the timer. */
    void reset(uint64_t totalBatches,
               uint64_t totalFrames,
               uint64_t initialBatchesCompleted = 0,
               uint64_t initialFramesCompleted = 0,
               double initialElapsedSeconds = 0.0)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const uint64_t clampedBatches = std::min(initialBatchesCompleted, totalBatches);
        const uint64_t clampedFrames = std::min(initialFramesCompleted, totalFrames);
        batchesCompleted_.store(clampedBatches, std::memory_order_relaxed);
        framesCompleted_.store(clampedFrames, std::memory_order_relaxed);
        validationPasses_.store(0, std::memory_order_relaxed);
        validationFails_.store(0, std::memory_order_relaxed);
        totalBatches_ = totalBatches;
        totalFrames_  = totalFrames;
        const double safeElapsed = std::max(0.0, initialElapsedSeconds);
        startTime_ = std::chrono::steady_clock::now()
                   - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                         std::chrono::duration<double>(safeElapsed));
    }

    /** Increment batch and frame counters (called by workers). */
    void recordBatch(uint64_t framesInBatch)
    {
        batchesCompleted_.fetch_add(1, std::memory_order_relaxed);
        framesCompleted_.fetch_add(framesInBatch, std::memory_order_relaxed);
    }

    void recordValidationPass()  { validationPasses_.fetch_add(1, std::memory_order_relaxed); }
    void recordValidationFail()  { validationFails_.fetch_add(1, std::memory_order_relaxed); }

    /** Get a consistent snapshot of progress. */
    Snapshot getSnapshot() const
    {
        Snapshot s;
        s.batchesCompleted = batchesCompleted_.load(std::memory_order_relaxed);
        s.framesCompleted  = framesCompleted_.load(std::memory_order_relaxed);
        s.validationPasses = validationPasses_.load(std::memory_order_relaxed);
        s.validationFails  = validationFails_.load(std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(mutex_);
            s.batchesTotal = totalBatches_;
            s.framesTotal  = totalFrames_;
        }

        auto now = std::chrono::steady_clock::now();
        s.elapsedSeconds = std::chrono::duration<double>(now - startTime_).count();

        if (s.elapsedSeconds > 0.0 && s.batchesCompleted > 0)
            s.batchesPerSecond = static_cast<double>(s.batchesCompleted) / s.elapsedSeconds;

        if (s.batchesTotal > 0)
            s.percentComplete = 100.0f * static_cast<float>(s.batchesCompleted)
                                       / static_cast<float>(s.batchesTotal);

        if (s.batchesPerSecond > 0.0 && s.batchesCompleted < s.batchesTotal)
        {
            uint64_t remaining = s.batchesTotal - s.batchesCompleted;
            s.etaSeconds = static_cast<double>(remaining) / s.batchesPerSecond;
        }

        return s;
    }

private:
    std::atomic<uint64_t> batchesCompleted_{0};
    std::atomic<uint64_t> framesCompleted_{0};
    std::atomic<uint64_t> validationPasses_{0};
    std::atomic<uint64_t> validationFails_{0};

    mutable std::mutex mutex_;
    uint64_t totalBatches_ = 0;
    uint64_t totalFrames_  = 0;
    std::chrono::steady_clock::time_point startTime_ = std::chrono::steady_clock::now();
};

} // namespace morphsnap
