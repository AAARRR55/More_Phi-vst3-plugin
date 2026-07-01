// src/AI/Agents/AnalysisAgent.h
#pragma once

#include "AI/Agents/IAgent.h"
#include "AI/Agents/AgentContext.h"

#include <atomic>
#include <future>
#include <mutex>
#include <vector>

namespace more_phi::agents {

// Read-only measurement & diagnosis. Never mutates. The system's eyes.
class AnalysisAgent : public IAgent
{
public:
    AgentRole role() const noexcept override { return AgentRole::Analysis; }
    juce::String id() const noexcept override { return "analysis-1"; }
    std::vector<juce::String> allowedTools() const override
    {
        return { "analysis.get_summary", "analysis.get_spectrum", "analysis.get_stereo_field",
                 "ozone.track.analyze", "get_mastering_state",
                 "analysis.capture_window", "analysis.compare_render",
                 // O1 (2026-06-29): dry-run neural mastering preview so the analysis
                 // agent can surface a model decision alongside live measurements.
                 "sonicmaster_decision" };
    }
    std::vector<juce::String> subscribedEventTypes() const override { return { "audio.transport_changed" }; }

    void prepare(const AgentContext& ctx) override { ctx_ = &ctx; }
    AgentResult execute(const AgentTask& task) override;
    AgentState state() const noexcept override { return state_.load(); }
    void stop() override { state_.store(AgentState::Stopped); }
    ~AnalysisAgent() override;

private:
    const AgentContext* ctx_ = nullptr;
    std::atomic<AgentState> state_{ AgentState::Idle };

    // O2-fix (2026-06-29): bounded collector for in-flight async analysis reads.
    // std::async futures BLOCK in their destructor, which made the per-read 5s
    // wait_for budget illusory (execute() could not return until every tool call
    // finished — a hung tool stalled a scheduler worker indefinitely) and gave no
    // bound on outstanding threads under re-entrant analysis tasks. We store
    // shared_futures (non-blocking destructor) here, drain completed ones before
    // launching more, apply backpressure at the cap (waiting on the oldest so
    // re-entrancy can't multiply the thread count unbounded), and block-drain the
    // rest in the destructor so no thread outlives the agent. All message-thread
    // domain — AnalysisAgent never runs on the audio thread.
    static constexpr size_t kMaxPendingReads = 8;
    void drainCompletedLocked();   // requires pendingMutex_ held
    std::mutex pendingMutex_;
    std::vector<std::shared_future<nlohmann::json>> pendingReads_;
};

} // namespace more_phi::agents
