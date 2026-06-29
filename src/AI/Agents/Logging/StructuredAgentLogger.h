// src/AI/Agents/Logging/StructuredAgentLogger.h
#pragma once

#include "AI/Agents/AgentContext.h"

#include <juce_core/juce_core.h>

#include <deque>
#include <fstream>
#include <mutex>

namespace more_phi::agents {

// IAgentLogger implementation that appends one JSON object per line (JSONL) to a
// per-run log file. The file path is resolved lazily on first log() call from
// the supplied directory + run id; if the file cannot be opened the logger
// degrades silently to in-memory buffering capped at kRingCapacity entries.
//
// Thread-safety: every log() call takes a mutex. Agents execute on scheduler
// workers (off the audio thread), so this locking is acceptable; the contract
// is "never on the audio thread" and StructuredAgentLogger respects it.
class StructuredAgentLogger : public IAgentLogger
{
public:
    StructuredAgentLogger(juce::File logDirectory, juce::String runId);

    void log(const juce::String& agentId,
             const juce::String& level,
             const juce::String& message,
             const nlohmann::json& fields = nlohmann::json::object()) override;

    // The active log file (empty until the first record is written).
    juce::File activeFile() const;

    // Number of records successfully flushed to disk (test surface).
    int flushedCount() const;

    // Number of records held in the in-memory ring because the file is closed.
    int bufferedCount() const;

    // Force the file to open immediately rather than lazily; returns false if the
    // file could not be created/opened (the logger then buffers in memory).
    bool openNow();

private:
    static constexpr int kRingCapacity = 256;

    void ensureOpenLocked();

    juce::File            directory_;
    juce::String          runId_;
    juce::File            file_;
    std::unique_ptr<std::ofstream> stream_;
    mutable std::mutex    mutex_;
    int                   flushed_ = 0;
    // Ring buffer of JSONL strings used when the file is unavailable.
    // ponytail: deque, not vector — pop_front is O(1). vector::erase(begin)
    // memoved 255 juce::String per push past the cap.
    std::deque<juce::String> ring_;
};

} // namespace more_phi::agents
