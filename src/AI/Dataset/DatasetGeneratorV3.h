#pragma once

#include "DatasetGeneratorV2.h"
#include "../../Core/LockFreeQueue.h"

#include "Scheduling/TaskQueue.h"
#include "Scheduling/WorkerPool.h"
#include "Monitoring/ResourceMonitor.h"
#include "Monitoring/ProgressTracker.h"
#include "Monitoring/GenerationLogger.h"
#include "Recovery/CheckpointManager.h"
#include "Recovery/WatchdogTimer.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace more_phi {

// Forward declarations — existing modules are referenced through DatasetGeneratorV2
struct DatasetGeneratorConfig;

/**
 * V3GenerationConfig — Extended configuration for the modular pipeline.
 * Inherits all V2 fields and adds pipeline-specific settings.
 */
struct V3GenerationConfig
{
    // --- Inherited from V2 ---
    DatasetGeneratorConfig baseConfig;

    // --- Pipeline settings ---
    int  batchSize            = 2048;     // Frames per batch
    int  workerThreads        = 0;        // 0 = auto (hardware_concurrency / 2)
    int  maxQueueDepth        = 4096;     // TaskQueue bounded capacity
    int  checkpointInterval   = 100;      // Checkpoint every N batches
    bool enableAdaptiveThrottle = true;   // ResourceMonitor-driven throttling
    bool enableWatchdog       = true;     // WatchdogTimer for hang detection

    // --- Watchdog ---
    std::chrono::milliseconds watchdogTimeout{5000};

    // --- Resource limits ---
    uint64_t memoryLimitBytes  = 2ULL * 1024 * 1024 * 1024; // 2 GB default
    float    maxQueueFillBeforeThrottle = 0.9f;

    // --- Output ---
    juce::File outputDirectory;
    juce::File logDirectory;
    juce::File checkpointDirectory;
};

/**
 * DatasetGeneratorV3 — Modular pipeline orchestrator.
 *
 * Builds on top of DatasetGeneratorV2's existing modules (ParameterSampler,
 * FeatureExtractor, ValidationEngine, etc.) and wraps them in a full
 * asynchronous pipeline with:
 *
 *   ParameterSampler ──SPSC──▶ DispatchThread ──MPMC──▶ WorkerPool
 *       (Capture)                (Batching)         (Extract+Transform+Validate)
 *                                                          │
 *                                                     ┌────▼────┐
 *                                             I/O Thread │Serialize│
 *                                                     └─────────┘
 *
 * Also integrates:
 *   - ResourceMonitor  for adaptive CPU/RAM throttling
 *   - CheckpointManager for crash recovery
 *   - WatchdogTimer     for hung-thread detection
 *   - ProgressTracker   for real-time progress & ETA
 *   - GenerationLogger  for structured JSON logging
 */
class DatasetGeneratorV3 : public juce::ChangeBroadcaster
{
public:
    DatasetGeneratorV3();
    ~DatasetGeneratorV3();

    // ── Configuration ──────────────────────────────────────────────────
    void setConfig(const V3GenerationConfig& config);
    const V3GenerationConfig& getConfig() const { return config_; }

    // ── Generation Lifecycle ───────────────────────────────────────────
    /** Start generation. Returns false if already running or config invalid. */
    bool startGeneration();

    /** Request graceful stop (completes current batch, serializes checkpoint). */
    void stopGeneration();

    /** Is the pipeline currently running? */
    bool isGenerating() const noexcept
    {
        return state_.load(std::memory_order_relaxed) == State::RUNNING;
    }

    /** Resume from the most recent checkpoint. */
    bool resumeFromCheckpoint();

    // ── Progress & Monitoring ──────────────────────────────────────────
    ProgressTracker::Snapshot getProgress() const { return progressTracker_.getSnapshot(); }
    ResourceSnapshot          getResourceSnapshot() const;

    // ── Callbacks ──────────────────────────────────────────────────────
    std::function<void(const ProgressTracker::Snapshot&)> onProgress;
    std::function<void(bool success, const std::string& message)> onComplete;

    // ── Module Access (advanced) ───────────────────────────────────────
    DatasetGeneratorV2&   getV2Module()           { return v2_; }
    TaskQueue&            getTaskQueue()           { return *taskQueue_; }
    WorkerPool&           getWorkerPool()          { return *workerPool_; }
    ResourceMonitor&      getResourceMonitor()     { return *resourceMonitor_; }
    ProgressTracker&      getProgressTracker()     { return progressTracker_; }
    GenerationLogger&     getLogger();
    CheckpointManager&    getCheckpointManager();

private:
    // ── Internal state machine ─────────────────────────────────────────
    enum class State { IDLE, STARTING, RUNNING, STOPPING };
    std::atomic<State> state_{State::IDLE};

    // ── Configuration ──────────────────────────────────────────────────
    V3GenerationConfig config_;

    // ── Underlying V2 module (owns ParameterSampler, FeatureExtractor, etc.) ──
    DatasetGeneratorV2 v2_;

    // ── Pipeline infrastructure (created on startGeneration) ───────────
    std::unique_ptr<TaskQueue>         taskQueue_;
    std::unique_ptr<WorkerPool>        workerPool_;
    std::unique_ptr<ResourceMonitor>   resourceMonitor_;
    std::unique_ptr<GenerationLogger>  logger_;
    std::unique_ptr<CheckpointManager> checkpointMgr_;
    std::unique_ptr<WatchdogTimer>     watchdog_;

    ProgressTracker progressTracker_;

    // ── Dispatch thread ────────────────────────────────────────────────
    std::unique_ptr<std::thread> dispatchThread_;

    /** Shared startup path used by both startGeneration() and resumeFromCheckpoint(). */
    bool initializeAndStart(uint64_t startBatchId,
                            uint64_t startFramesProcessed,
                            double startElapsedSeconds,
                            bool resumedFromCheckpoint);

    /** Main dispatch loop: batches captured data and submits to TaskQueue. */
    void dispatchLoop(uint64_t startBatchId, uint64_t startFramesDispatched);

    /** Process a single batch (runs inside a worker thread). */
    void processBatch(uint64_t batchId,
                      std::vector<std::vector<float>> parameterBatch);

    /** Finalize: flush remaining data, clear checkpoint, fire onComplete. */
    void finalize(bool success, const std::string& message);

    // ── Adaptive throttling ────────────────────────────────────────────
    int  adaptiveBatchSize() const;
    int  adaptiveSleepMs() const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DatasetGeneratorV3)
};

} // namespace more_phi
