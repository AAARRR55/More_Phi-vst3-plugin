// src/AI/Agents/Agents/AnalysisAgent.cpp
#include "AI/Agents/Agents/AnalysisAgent.h"

namespace more_phi::agents {

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

    auto summary = ctx_->tools->invoke("analysis.get_summary", {}, id());
    if (! summary.contains("error"))
        findings["summary"] = summary;

    auto spectrum = ctx_->tools->invoke("analysis.get_spectrum", {}, id());
    if (! spectrum.contains("error"))
        findings["spectrum"] = spectrum;

    auto stereo = ctx_->tools->invoke("analysis.get_stereo_field", {}, id());
    if (! stereo.contains("error"))
        findings["stereo"] = stereo;

    // Flatten headline numbers for event consumers (reactive agents).
    if (summary.contains("lufs_integrated")) findings["lufs_integrated"] = summary["lufs_integrated"];
    if (summary.contains("true_peak_db"))    findings["true_peak_db"]    = summary["true_peak_db"];
    if (spectrum.contains("tilt_db"))        findings["tilt_db"]         = spectrum["tilt_db"];

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
