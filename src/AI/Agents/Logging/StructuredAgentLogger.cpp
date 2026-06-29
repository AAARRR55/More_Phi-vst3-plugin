// src/AI/Agents/Logging/StructuredAgentLogger.cpp
#include "AI/Agents/Logging/StructuredAgentLogger.h"

#include <juce_core/juce_core.h>

namespace more_phi::agents {

StructuredAgentLogger::StructuredAgentLogger(juce::File logDirectory, juce::String runId)
    : directory_(std::move(logDirectory))
    , runId_(std::move(runId))
{
}

void StructuredAgentLogger::log(const juce::String& agentId,
                                 const juce::String& level,
                                 const juce::String& message,
                                 const nlohmann::json& fields)
{
    const auto tsMs = juce::Time::currentTimeMillis();

    nlohmann::json record = nlohmann::json::object();
    record["ts_ms"]   = tsMs;
    record["run_id"]  = runId_.toStdString();
    record["agent"]   = agentId.toStdString();
    record["level"]   = level.toStdString();
    record["message"] = message.toStdString();
    record["fields"]  = fields.is_object() ? fields : nlohmann::json::object();

    const juce::String line = juce::String(record.dump());

    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ == nullptr)
        ensureOpenLocked();

    if (stream_ != nullptr && stream_->is_open())
    {
        *stream_ << line.toStdString() << '\n';
        stream_->flush();
        ++flushed_;
        return;
    }

    // File unavailable → keep the most recent kRingCapacity JSONL lines in memory
    // so a later flush (or a test) can still inspect the trail.
    if (static_cast<int>(ring_.size()) >= kRingCapacity)
        ring_.pop_front();
    ring_.push_back(std::move(line));
}

juce::File StructuredAgentLogger::activeFile() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return file_;
}

int StructuredAgentLogger::flushedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return flushed_;
}

int StructuredAgentLogger::bufferedCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(ring_.size());
}

bool StructuredAgentLogger::openNow()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_ != nullptr)
        return stream_->is_open();
    ensureOpenLocked();
    return stream_ != nullptr && stream_->is_open();
}

void StructuredAgentLogger::ensureOpenLocked()
{
    if (stream_ != nullptr)
        return;

    if (runId_.trim().isEmpty())
        runId_ = "default";

    if (directory_ == juce::File() || directory_.getFullPathName().isEmpty())
    {
        // No directory supplied → degrade to buffering (e.g. unit tests).
        return;
    }

    if (! directory_.exists())
    {
        juce::Result r = directory_.createDirectory();
        if (r.failed())
            return;
    }

    const auto safeRunId = runId_.replaceCharacters("\\/:*?\"<>|", "_________");
    file_ = directory_.getChildFile("agents-" + safeRunId + ".jsonl");

    auto out = std::make_unique<std::ofstream>(file_.getFullPathName().toStdString(),
                                                std::ios::out | std::ios::app);
    if (out->is_open())
        stream_ = std::move(out);
    // If we could not open the file, leave stream_ null → ring buffer is used.
}

} // namespace more_phi::agents
