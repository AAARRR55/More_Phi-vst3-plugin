#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <mutex>

namespace more_phi {

/**
 * Checkpoint — Serializable snapshot of generation state for crash recovery.
 */
struct Checkpoint
{
    uint64_t batchId              = 0;
    uint64_t totalFramesProcessed = 0;
    uint64_t totalFramesTarget    = 0;
    std::string lastOutputFile;
    std::string outputDirectory;
    int batchSize                 = 2048;
    int workerThreads             = 2;
    double elapsedSeconds         = 0.0;
    std::string configHash;       // Hash of the generation config for consistency check

    nlohmann::json toJson() const
    {
        return {
            {"batchId",              batchId},
            {"totalFramesProcessed", totalFramesProcessed},
            {"totalFramesTarget",    totalFramesTarget},
            {"lastOutputFile",       lastOutputFile},
            {"outputDirectory",      outputDirectory},
            {"batchSize",            batchSize},
            {"workerThreads",        workerThreads},
            {"elapsedSeconds",       elapsedSeconds},
            {"configHash",           configHash}
        };
    }

    static Checkpoint fromJson(const nlohmann::json& j)
    {
        Checkpoint c;
        c.batchId              = j.value("batchId", uint64_t(0));
        c.totalFramesProcessed = j.value("totalFramesProcessed", uint64_t(0));
        c.totalFramesTarget    = j.value("totalFramesTarget", uint64_t(0));
        c.lastOutputFile       = j.value("lastOutputFile", std::string());
        c.outputDirectory      = j.value("outputDirectory", std::string());
        c.batchSize            = j.value("batchSize", 2048);
        c.workerThreads        = j.value("workerThreads", 2);
        c.elapsedSeconds       = j.value("elapsedSeconds", 0.0);
        c.configHash           = j.value("configHash", std::string());
        return c;
    }
};

/**
 * CheckpointManager — Saves and restores generation state for crash recovery.
 *
 * Checkpoints are written to a JSON file at configurable intervals
 * (default: every 100 batches). On restart, the pipeline can resume
 * from the last successful checkpoint rather than starting over.
 */
class CheckpointManager
{
public:
    /**
     * @param checkpointDir  Directory to store checkpoint files.
     * @param intervalBatches  How often to checkpoint (in batches).
     */
    CheckpointManager(const juce::File& checkpointDir, int intervalBatches = 100)
        : checkpointDir_(checkpointDir), intervalBatches_(intervalBatches)
    {
        checkpointDir_.createDirectory();
    }

    /** Should a checkpoint be taken now? */
    bool shouldCheckpoint(uint64_t currentBatchId) const noexcept
    {
        return intervalBatches_ > 0 && (currentBatchId % intervalBatches_ == 0);
    }

    /** Save current state to disk. Returns true on success. */
    bool save(const Checkpoint& cp)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto file = checkpointDir_.getChildFile("checkpoint.json");
        auto json = cp.toJson();
        json["savedAt"] = juce::Time::getCurrentTime().toISO8601(true).toStdString();

        return file.replaceWithText(json.dump(2));
    }

    /** Load the most recent checkpoint. Returns nullopt if none exists. */
    std::optional<Checkpoint> load() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto file = checkpointDir_.getChildFile("checkpoint.json");
        if (!file.existsAsFile())
            return std::nullopt;

        try
        {
            auto text = file.loadFileAsString().toStdString();
            auto j = nlohmann::json::parse(text);
            return Checkpoint::fromJson(j);
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    /** Delete the checkpoint file (e.g., after successful completion). */
    void clear()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto file = checkpointDir_.getChildFile("checkpoint.json");
        file.deleteFile();
    }

    /** Get the interval in batches. */
    int getInterval() const noexcept { return intervalBatches_; }

    /** Set the interval in batches. */
    void setInterval(int batches) { intervalBatches_ = batches; }

private:
    juce::File checkpointDir_;
    int intervalBatches_;
    mutable std::mutex mutex_;
};

} // namespace more_phi
