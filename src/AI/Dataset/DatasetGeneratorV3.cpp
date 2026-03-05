#include "DatasetGeneratorV3.h"
#include <algorithm>
#include <cmath>

namespace morphsnap {

// ============================================================================
// Construction / Destruction
// ============================================================================

DatasetGeneratorV3::DatasetGeneratorV3() = default;

DatasetGeneratorV3::~DatasetGeneratorV3()
{
    stopGeneration();
}

// ============================================================================
// Configuration
// ============================================================================

void DatasetGeneratorV3::setConfig(const V3GenerationConfig& config)
{
    jassert(state_.load(std::memory_order_relaxed) == State::IDLE);
    config_ = config;

    // Forward base config to V2 module
    v2_.setConfig(config.baseConfig);
}

// ============================================================================
// Generation Lifecycle
// ============================================================================

bool DatasetGeneratorV3::startGeneration()
{
    State expected = State::IDLE;
    if (!state_.compare_exchange_strong(expected, State::STARTING,
                                        std::memory_order_acq_rel))
        return false; // Already running
    return initializeAndStart(0, 0, 0.0, false);
}

bool DatasetGeneratorV3::initializeAndStart(uint64_t startBatchId,
                                            uint64_t startFramesProcessed,
                                            double startElapsedSeconds,
                                            bool resumedFromCheckpoint)
{
    // --- Validate config ---
    juce::String error;
    if (!DatasetGeneratorV2::validateConfig(config_.baseConfig, error))
    {
        state_.store(State::IDLE, std::memory_order_release);
        if (onComplete)
            onComplete(false, "Config validation failed: " + error.toStdString());
        return false;
    }

    // --- Ensure output directories exist ---
    if (config_.outputDirectory.getFullPathName().isNotEmpty())
        config_.outputDirectory.createDirectory();

    auto logDir = config_.logDirectory.getFullPathName().isNotEmpty()
                    ? config_.logDirectory
                    : config_.outputDirectory.getChildFile("logs");

    auto cpDir = config_.checkpointDirectory.getFullPathName().isNotEmpty()
                   ? config_.checkpointDirectory
                   : config_.outputDirectory.getChildFile("checkpoints");

    // Tear down stale infrastructure (if any) before rebuilding.
    if (dispatchThread_ && dispatchThread_->joinable())
    {
        state_.store(State::STOPPING, std::memory_order_release);
        dispatchThread_->join();
    }
    dispatchThread_.reset();
    if (watchdog_)
        watchdog_->stop();
    if (workerPool_)
        workerPool_->stop();
    if (resourceMonitor_)
        resourceMonitor_->stop();

    taskQueue_.reset();
    workerPool_.reset();
    resourceMonitor_.reset();
    logger_.reset();
    checkpointMgr_.reset();
    watchdog_.reset();

    // --- Create infrastructure ---
    taskQueue_       = std::make_unique<TaskQueue>(static_cast<size_t>(config_.maxQueueDepth));
    workerPool_      = std::make_unique<WorkerPool>(*taskQueue_, config_.workerThreads);
    resourceMonitor_ = std::make_unique<ResourceMonitor>(std::chrono::milliseconds(1000),
                                                         config_.memoryLimitBytes);
    logger_          = std::make_unique<GenerationLogger>(logDir);
    checkpointMgr_   = std::make_unique<CheckpointManager>(cpDir, config_.checkpointInterval);

    // Wire up resource monitor queue fill provider
    resourceMonitor_->setQueueFillProvider([this]() -> float {
        return taskQueue_ ? taskQueue_->fillRatio() : 0.0f;
    });

    // Wire up worker error handler
    workerPool_->setErrorHandler([this](const std::string& msg) {
        if (logger_)
            logger_->log(LogLevel::ERROR_, "Worker exception: " + msg);
    });

    // --- Progress tracker ---
    const uint64_t totalFrames = static_cast<uint64_t>(juce::jmax(config_.baseConfig.totalSamples, 0));
    const uint64_t totalBatches = (totalFrames > 0 && config_.batchSize > 0)
        ? (totalFrames + static_cast<uint64_t>(config_.batchSize) - 1) / static_cast<uint64_t>(config_.batchSize)
        : 0;
    const uint64_t clampedStartBatch = std::min(startBatchId, totalBatches);
    const uint64_t clampedStartFrames = std::min(startFramesProcessed, totalFrames);
    progressTracker_.reset(totalBatches,
                           totalFrames,
                           clampedStartBatch,
                           clampedStartFrames,
                           std::max(0.0, startElapsedSeconds));

    // --- Watchdog ---
    if (config_.enableWatchdog)
    {
        watchdog_ = std::make_unique<WatchdogTimer>(
            config_.watchdogTimeout,
            [this]() {
                if (logger_)
                    logger_->log(LogLevel::WARN, "Watchdog timeout — worker may be hung");
            }
        );
        watchdog_->start();
    }

    // --- Start subsystems ---
    resourceMonitor_->start();
    workerPool_->start();

    if (logger_)
    {
        logger_->log(LogLevel::INFO,
                     resumedFromCheckpoint ? "Generation resumed from checkpoint" : "Generation started",
                     nlohmann::json({
                         {"batchSize",     config_.batchSize},
                         {"workerThreads", workerPool_->threadCount()},
                         {"totalSamples",  config_.baseConfig.totalSamples},
                         {"startBatchId",  clampedStartBatch},
                         {"startFrames",   clampedStartFrames}
                     }));
    }

    // --- Launch dispatch thread ---
    state_.store(State::RUNNING, std::memory_order_release);
    dispatchThread_ = std::make_unique<std::thread>(
        [this, clampedStartBatch, clampedStartFrames]() {
            dispatchLoop(clampedStartBatch, clampedStartFrames);
        });

    return true;
}

void DatasetGeneratorV3::stopGeneration()
{
    State expected = State::RUNNING;
    if (!state_.compare_exchange_strong(expected, State::STOPPING,
                                        std::memory_order_acq_rel))
        return;

    // Let dispatch loop exit naturally
    if (dispatchThread_ && dispatchThread_->joinable())
        dispatchThread_->join();
    dispatchThread_.reset();

    // Stop subsystems
    if (watchdog_)
        watchdog_->stop();
    if (workerPool_)
        workerPool_->stop();
    if (resourceMonitor_)
        resourceMonitor_->stop();

    // Save final checkpoint
    if (checkpointMgr_)
    {
        auto snap = progressTracker_.getSnapshot();
        Checkpoint cp;
        cp.batchId              = snap.batchesCompleted;
        cp.totalFramesProcessed = snap.framesCompleted;
        cp.totalFramesTarget    = snap.framesTotal;
        cp.elapsedSeconds       = snap.elapsedSeconds;
        checkpointMgr_->save(cp);
    }

    if (logger_)
        logger_->log(LogLevel::INFO, "Generation stopped by user");

    state_.store(State::IDLE, std::memory_order_release);
}

bool DatasetGeneratorV3::resumeFromCheckpoint()
{
    auto cpDir = config_.checkpointDirectory.getFullPathName().isNotEmpty()
                   ? config_.checkpointDirectory
                   : config_.outputDirectory.getChildFile("checkpoints");
    CheckpointManager reader(cpDir, config_.checkpointInterval);
    auto cp = reader.load();
    if (!cp.has_value())
        return false;

    State expected = State::IDLE;
    if (!state_.compare_exchange_strong(expected, State::STARTING,
                                        std::memory_order_acq_rel))
        return false;

    return initializeAndStart(cp->batchId,
                              cp->totalFramesProcessed,
                              cp->elapsedSeconds,
                              true);
}

// ============================================================================
// Dispatch Loop
// ============================================================================

void DatasetGeneratorV3::dispatchLoop(uint64_t startBatchId, uint64_t startFramesDispatched)
{
    auto& sampler = v2_.getSampler();
    const int paramCount = config_.baseConfig.totalSamples > 0
        ? static_cast<int>(config_.baseConfig.totalSamples) : 100;
    juce::ignoreUnused(paramCount);

    uint64_t batchId = startBatchId;
    uint64_t totalFramesNeeded = static_cast<uint64_t>(config_.baseConfig.totalSamples);
    uint64_t framesDispatched = std::min(startFramesDispatched, totalFramesNeeded);

    while (state_.load(std::memory_order_acquire) == State::RUNNING
           && framesDispatched < totalFramesNeeded)
    {
        // --- Adaptive batch size based on system load ---
        int currentBatchSize = adaptiveBatchSize();

        // Don't exceed remaining needed frames
        uint64_t remaining = totalFramesNeeded - framesDispatched;
        if (static_cast<uint64_t>(currentBatchSize) > remaining)
            currentBatchSize = static_cast<int>(remaining);

        // --- Generate parameter samples for this batch ---
        SamplingConfig samplingCfg;
        samplingCfg.sampleCount = currentBatchSize;
        auto batch = sampler.generateLHS(samplingCfg,
                                          config_.baseConfig.totalSamples);

        // --- Submit to task queue ---
        uint64_t thisBatchId = batchId;
        auto batchCopy = std::move(batch);

        PipelineTask task;
        task.batchId  = thisBatchId;
        task.priority = TaskPriority::NORMAL;
        task.work = [this, thisBatchId, b = std::move(batchCopy)]() mutable {
            processBatch(thisBatchId, std::move(b));
        };

        if (!taskQueue_->push(std::move(task)))
            break; // Queue shut down

        framesDispatched += currentBatchSize;
        batchId++;

        // --- Kick watchdog ---
        if (watchdog_)
            watchdog_->kick();

        // --- Adaptive sleep to prevent overloading ---
        int sleepMs = adaptiveSleepMs();
        if (sleepMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));

        // --- Periodic checkpoint ---
        if (checkpointMgr_ && checkpointMgr_->shouldCheckpoint(batchId))
        {
            Checkpoint cp;
            cp.batchId              = batchId;
            cp.totalFramesProcessed = framesDispatched;
            cp.totalFramesTarget    = totalFramesNeeded;
            cp.batchSize            = currentBatchSize;
            checkpointMgr_->save(cp);

            if (logger_)
                logger_->log(LogLevel::INFO, "Checkpoint saved", nlohmann::json({{"batchId", batchId}}));
        }

        // --- Periodic progress report ---
        if (batchId % 10 == 0)
        {
            auto snap = progressTracker_.getSnapshot();
            if (logger_)
                logger_->logProgress(snap);
            if (onProgress)
                onProgress(snap);
        }
    }

    // --- All batches dispatched; wait for workers to finish ---
    // The TaskQueue draining will be handled by WorkerPool
    // We do a soft wait by checking progress
    while (state_.load(std::memory_order_relaxed) == State::RUNNING)
    {
        auto snap = progressTracker_.getSnapshot();
        if (snap.batchesCompleted >= snap.batchesTotal)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    bool success = (state_.load(std::memory_order_relaxed) == State::RUNNING);
    finalize(success, success ? "Generation complete" : "Generation stopped");
}

// ============================================================================
// Batch Processing (runs on worker threads)
// ============================================================================

void DatasetGeneratorV3::processBatch(uint64_t batchId,
                                       std::vector<std::vector<float>> parameterBatch)
{
    auto& extractor  = v2_.getFeatureExtractor();
    auto& validator  = v2_.getValidationEngine();
    auto& writer     = v2_.getMetadataWriter();

    // --- Validate parameter ranges (NaN, Inf, [0,1] range) ---
    bool allValid = true;
    for (auto& params : parameterBatch)
    {
        for (float& v : params)
        {
            if (std::isnan(v) || std::isinf(v))
            {
                allValid = false;
                v = 0.0f; // Sanitize
            }
            v = std::clamp(v, 0.0f, 1.0f);
        }
    }

    if (!allValid)
        progressTracker_.recordValidationFail();
    else
        progressTracker_.recordValidationPass();

    // --- Serialize batch to disk ---
    // (Future: write HDF5/Parquet. For now, use MetadataWriter JSON)
    if (config_.outputDirectory.getFullPathName().isNotEmpty())
    {
        auto batchFile = config_.outputDirectory.getChildFile(
            juce::String("batch_") + juce::String(batchId) + ".json");

        nlohmann::json batchJson;
        batchJson["batchId"] = batchId;
        batchJson["frameCount"] = parameterBatch.size();

        nlohmann::json samplesArray = nlohmann::json::array();
        for (const auto& params : parameterBatch)
            samplesArray.push_back(params);
        batchJson["parameters"] = samplesArray;

        batchFile.replaceWithText(batchJson.dump());
    }

    // --- Update progress ---
    progressTracker_.recordBatch(static_cast<uint64_t>(parameterBatch.size()));

    // --- Kick watchdog ---
    if (watchdog_)
        watchdog_->kick();
}

// ============================================================================
// Finalization
// ============================================================================

void DatasetGeneratorV3::finalize(bool success, const std::string& message)
{
    if (logger_)
    {
        auto snap = progressTracker_.getSnapshot();
        logger_->logProgress(snap);
        logger_->log(success ? LogLevel::INFO : LogLevel::WARN,
                     "Finalized: " + message);
    }

    if (success && checkpointMgr_)
        checkpointMgr_->clear(); // Clean up checkpoint on success

    state_.store(State::IDLE, std::memory_order_release);

    if (onComplete)
        onComplete(success, message);

    sendChangeMessage(); // JUCE notification
}

// ============================================================================
// Adaptive Throttling
// ============================================================================

int DatasetGeneratorV3::adaptiveBatchSize() const
{
    if (!config_.enableAdaptiveThrottle || !resourceMonitor_)
        return config_.batchSize;

    switch (resourceMonitor_->getLoadLevel())
    {
        case SystemLoadLevel::LOW:      return config_.batchSize;
        case SystemLoadLevel::MEDIUM:   return config_.batchSize / 2;
        case SystemLoadLevel::HIGH:     return config_.batchSize / 4;
        case SystemLoadLevel::CRITICAL: return juce::jmax(config_.batchSize / 8, 64);
    }
    return config_.batchSize;
}

int DatasetGeneratorV3::adaptiveSleepMs() const
{
    if (!config_.enableAdaptiveThrottle || !resourceMonitor_)
        return 0;

    switch (resourceMonitor_->getLoadLevel())
    {
        case SystemLoadLevel::LOW:      return 0;
        case SystemLoadLevel::MEDIUM:   return 5;
        case SystemLoadLevel::HIGH:     return 20;
        case SystemLoadLevel::CRITICAL: return 50;
    }
    return 0;
}

// ============================================================================
// Module Access
// ============================================================================

ResourceSnapshot DatasetGeneratorV3::getResourceSnapshot() const
{
    return resourceMonitor_ ? resourceMonitor_->getSnapshot() : ResourceSnapshot{};
}

GenerationLogger& DatasetGeneratorV3::getLogger()
{
    jassert(logger_ != nullptr);
    return *logger_;
}

CheckpointManager& DatasetGeneratorV3::getCheckpointManager()
{
    jassert(checkpointMgr_ != nullptr);
    return *checkpointMgr_;
}

} // namespace morphsnap
