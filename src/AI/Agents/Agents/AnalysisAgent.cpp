// src/AI/Agents/Agents/AnalysisAgent.cpp
#include "AI/Agents/Agents/AnalysisAgent.h"

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

    // O2 (2026-06-29): the three analysis.get_* reads have no data dependency
    // between them — run them concurrently instead of serially. Each dispatches
    // through MCPToolHandler (parse + lock + resolve + serialize), so the serial
    // version paid 3x the per-call overhead. std::async spawns transient threads;
    // this is safe because execute() runs on a scheduler worker (message-thread
    // domain, NEVER the audio thread) and AnalysisAgent is goal-driven, not
    // per-block. A per-call timeout guards against a hung tool call blocking the
    // worker indefinitely (mirrors the masteringNeuralApply 5s budget).
    auto launchRead = [this](const char* tool) -> std::future<nlohmann::json> {
        return std::async(std::launch::async, [this, tool] {
            return ctx_->tools->invoke(tool, {}, id());
        });
    };
    auto summaryFut  = launchRead("analysis.get_summary");
    auto spectrumFut = launchRead("analysis.get_spectrum");
    auto stereoFut   = launchRead("analysis.get_stereo_field");

    // Helper: wait with a 5s budget, return {} on timeout/exception.
    auto awaitWithBudget = [](std::future<nlohmann::json>& f) -> nlohmann::json {
        try
        {
            if (f.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
                return f.get();
        }
        catch (...) {}
        return nlohmann::json::object();
    };

    const auto summary  = awaitWithBudget(summaryFut);
    const auto spectrum = awaitWithBudget(spectrumFut);
    const auto stereo   = awaitWithBudget(stereoFut);

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
