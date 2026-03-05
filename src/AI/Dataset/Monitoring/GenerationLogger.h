#pragma once

#include "ProgressTracker.h"
#include "ResourceMonitor.h"

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>

namespace morphsnap {

/**
 * LogLevel for structured logging.
 */
enum class LogLevel
{
    DEBUG_,  // Trailing underscore to avoid macro collision
    INFO,
    WARN,
    ERROR_
};

inline const char* logLevelToString(LogLevel l)
{
    switch (l)
    {
        case LogLevel::DEBUG_: return "DEBUG";
        case LogLevel::INFO:   return "INFO";
        case LogLevel::WARN:   return "WARN";
        case LogLevel::ERROR_: return "ERROR";
    }
    return "UNKNOWN";
}

/**
 * GenerationLogger — Structured JSON logging for dataset generation.
 *
 * Writes one JSON object per line (JSON Lines format) for easy
 * machine parsing and log aggregation.
 *
 * Log rotation: creates a new file when the current file exceeds maxFileSizeBytes.
 */
class GenerationLogger
{
public:
    /**
     * @param logDir           Directory for log files.
     * @param maxFileSizeBytes Maximum size per log file before rotation (default 100 MB).
     * @param maxFiles         Maximum number of retained log files (default 10).
     */
    GenerationLogger(const juce::File& logDir,
                     size_t maxFileSizeBytes = 100 * 1024 * 1024,
                     int maxFiles = 10)
        : logDir_(logDir),
          maxFileSizeBytes_(maxFileSizeBytes),
          maxFiles_(maxFiles)
    {
        logDir_.createDirectory();
        openNewLogFile();
    }

    ~GenerationLogger()
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (stream_.is_open())
            stream_.close();
    }

    GenerationLogger(const GenerationLogger&) = delete;
    GenerationLogger& operator=(const GenerationLogger&) = delete;

    /** Log a message at the given level. */
    void log(LogLevel level, const std::string& message,
             const nlohmann::json& extra = {})
    {
        nlohmann::json entry;
        entry["ts"]    = juce::Time::getCurrentTime().toISO8601(true).toStdString();
        entry["level"] = logLevelToString(level);
        entry["msg"]   = message;

        if (!extra.is_null() && !extra.empty())
            entry["data"] = extra;

        write(entry);
    }

    /** Log a periodic progress snapshot. */
    void logProgress(const ProgressTracker::Snapshot& snap)
    {
        nlohmann::json data;
        data["batchesCompleted"] = snap.batchesCompleted;
        data["batchesTotal"]     = snap.batchesTotal;
        data["percentComplete"]  = snap.percentComplete;
        data["batchesPerSec"]    = snap.batchesPerSecond;
        data["elapsedSec"]       = snap.elapsedSeconds;
        data["etaSec"]           = snap.etaSeconds;
        data["validationPasses"] = snap.validationPasses;
        data["validationFails"]  = snap.validationFails;

        log(LogLevel::INFO, "progress", data);
    }

    /** Log a resource monitor snapshot. */
    void logResources(const ResourceSnapshot& snap)
    {
        nlohmann::json data;
        data["cpuPercent"]  = snap.cpuUsagePercent;
        data["memPercent"]  = snap.memUsagePercent;
        data["memUsedMB"]   = snap.memUsedBytes / (1024 * 1024);
        data["queueFill"]   = snap.queueFillRatio;
        data["loadLevel"]   = static_cast<int>(snap.level);

        log(LogLevel::INFO, "resources", data);
    }

private:
    void write(const nlohmann::json& entry)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!stream_.is_open())
            return;

        std::string line = entry.dump(-1) + "\n"; // Single-line JSON
        stream_ << line;
        stream_.flush();
        currentFileSize_ += line.size();

        if (currentFileSize_ >= maxFileSizeBytes_)
            rotate();
    }

    void openNewLogFile()
    {
        auto timestamp = juce::Time::getCurrentTime()
                             .formatted("%Y%m%d_%H%M%S")
                             .toStdString();
        auto file = logDir_.getChildFile(
            juce::String("generation_") + timestamp + ".jsonl");

        stream_.open(file.getFullPathName().toStdString(), std::ios::app);
        currentFileSize_ = 0;
    }

    void rotate()
    {
        if (stream_.is_open())
            stream_.close();

        // Delete oldest files if we exceed maxFiles_
        auto files = logDir_.findChildFiles(juce::File::findFiles, false, "*.jsonl");
        files.sort();
        while (files.size() >= maxFiles_)
        {
            files.getFirst().deleteFile();
            files.remove(0);
        }

        openNewLogFile();
    }

    juce::File logDir_;
    size_t maxFileSizeBytes_;
    int maxFiles_;

    std::mutex mutex_;
    std::ofstream stream_;
    size_t currentFileSize_ = 0;
};

} // namespace morphsnap
