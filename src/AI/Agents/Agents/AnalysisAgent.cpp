// src/AI/Agents/Agents/AnalysisAgent.cpp
#include "AI/Agents/Agents/AnalysisAgent.h"

#include <algorithm>
#include <future>

namespace more_phi::agents {

namespace {
// O1 (2026-06-29): only pull a (relatively expensive) neural decision preview when
// the task is mastering-oriented. Avoids redundant ONNX inference on every analysis
// run (e.g. transport-change re-analyses).
bool isMasteringIntent(const juce::String& intent)
{
    const auto s = intent.toLowerCase().toStdString();
    return s.find("master") != std::string::npos
        || s.find("loudness") != std::string::npos
        || s.find("lufs") != std::string::npos
        || s.find("streaming") != std::string::npos
        || s.find("polish") != std::string::npos;
}
} // namespace

AnalysisAgent::~AnalysisAgent()
{
    // Block until every in-flight read finishes so no tool thread touches a freed
    // agent context. Safe: the registry destroys agents only after the scheduler
    // and pump are stopped (message-thread domain, never the audio thread).
    std::vector<std::shared_future<nlohmann::json>> snapshot;
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        snapshot.swap(pendingReads_);
    }
    for (auto& f : snapshot)
    {
        try { f.wait(); } catch (...) {}
    }
}

void AnalysisAgent::drainCompletedLocked()
{
    auto& v = pendingReads_;
    v.erase(std::remove_if(v.begin(), v.end(),
        [](const std::shared_future<nlohmann::json>& f) {
            return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }), v.end());
}

AgentResult AnalysisAgent::execute(const AgentTask& task)
{
    state_.store(AgentState::Busy);
    AgentResult r;
    r.taskId = task.id;

    if (! ctx_ || ! ctx_->tools)
    {
        r.success = false;
        r.errorCode = "no_tool_invoker";
        state_.store(AgentState::Idle);
        return r;
    }

    nlohmann::json findings = nlohmann::json::object();
    findings["intent"] = task.intent.toStdString();

    // O2 (2026-06-29) + O2-fix: the three analysis.get_* reads have no data
    // dependency — run them concurrently via std::async. Each dispatches through
    // MCPToolHandler (parse + lock + resolve + serialize), so serially they paid
    // 3x the per-call overhead. Safe: execute() runs on a scheduler worker
    // (message-thread domain, NEVER the audio thread) and AnalysisAgent is
    // goal-driven, not per-block.
    //
    // O2-fix: std::async futures BLOCK in their destructor, so the previous
    // version's 5s wait_for was illusory — execute() could not return until every
    // tool call finished, and a hung tool stalled the worker indefinitely. We now
    // keep the in-flight reads as shared_futures (non-blocking destructor) in a
    // bounded collector, drain completed ones before launching more, and apply
    // backpressure at the cap so re-entrant execute() calls can't multiply the
    // thread count unbounded. The destructor block-drains the rest.
    auto launchRead = [this](const char* tool) -> std::shared_future<nlohmann::json> {
        for (;;)
        {
            std::shared_future<nlohmann::json> oldest;
            {
                std::lock_guard<std::mutex> lock(pendingMutex_);
                drainCompletedLocked();
                if (pendingReads_.size() < kMaxPendingReads)
                    break;
                oldest = pendingReads_.front();   // backpressure: wait on oldest outside the lock
            }
            try { oldest.wait(); } catch (...) {}
        }
        auto fut = std::async(std::launch::async, [this, tool] {
            return ctx_->tools->invoke(tool, {}, id());
        });
        auto shared = fut.share();
        {
            std::lock_guard<std::mutex> lock(pendingMutex_);
            drainCompletedLocked();
            pendingReads_.push_back(shared);
        }
        return shared;
    };
    auto summarySf  = launchRead("analysis.get_summary");
    auto spectrumSf = launchRead("analysis.get_spectrum");
    auto stereoSf   = launchRead("analysis.get_stereo_field");

    // Helper: wait with a 5s budget, return {} on timeout/exception. Because the
    // futures are shared (non-blocking destructor), a timeout now actually lets
    // execute() return promptly; the read keeps running and is drained later / by
    // the destructor.
    auto awaitWithBudget = [](std::shared_future<nlohmann::json>& f) -> nlohmann::json {
        try
        {
            if (f.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
                return f.get();
        }
        catch (...) {}
        return nlohmann::json::object();
    };

    const auto summary  = awaitWithBudget(summarySf);
    const auto spectrum = awaitWithBudget(spectrumSf);
    const auto stereo   = awaitWithBudget(stereoSf);

    if (! summary.contains("error"))  findings["summary"]  = summary;
    if (! spectrum.contains("error")) findings["spectrum"] = spectrum;
    if (! stereo.contains("error"))   findings["stereo"]   = stereo;

    // Flatten headline numbers for event consumers (reactive agents).
    if (summary.contains("lufs_integrated")) findings["lufs_integrated"] = summary["lufs_integrated"];
    if (summary.contains("true_peak_db"))    findings["true_peak_db"]    = summary["true_peak_db"];
    if (spectrum.contains("tilt_db"))        findings["tilt_db"]         = spectrum["tilt_db"];

    // O1 (2026-06-29): for mastering intents, also pull a dry-run neural decision
    // so the analysis includes the model's recommendation alongside live meters.
    if (isMasteringIntent(task.intent))
    {
        auto decision = ctx_->tools->invoke("sonicmaster_decision",
            { { "target_lufs", task.payload.value("target_lufs", -14.0) } }, id());
        if (! decision.contains("error") && decision.value("success", false))
            findings["neural_decision"] = decision;
    }

    r.findings = findings;
    r.success = true;

    r.emitEvents.push_back({ "analysis.finding", findings });

    // Reactive triggers (consumed by RealtimeControlAgent):
    if (summary.contains("true_peak_db") && summary["true_peak_db"].is_number())
    {
        const double tp = summary["true_peak_db"].get<double>();
        if (tp > -0.1)
            r.emitEvents.push_back({ "analysis.clipping_detected",
                { { "true_peak_db", tp }, { "source", id().toStdString() } } });
    }
    if (summary.contains("lufs_integrated") && summary["lufs_integrated"].is_number())
    {
        const double lufs = summary["lufs_integrated"].get<double>();
        if (lufs > -8.0)
            r.emitEvents.push_back({ "analysis.lufs_breach",
                { { "lufs_integrated", lufs }, { "source", id().toStdString() } } });
    }

    state_.store(AgentState::Idle);
    return r;
}

} // namespace more_phi::agents
