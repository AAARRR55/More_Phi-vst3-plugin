/*
 * More-Phi — AI/MCPToolHandler.h
 * Dispatches MCP tool calls to plugin subsystems.
 * Instance-aware: includes identity context in responses.
 */
#pragma once

#include <juce_core/juce_core.h>

namespace more_phi {

class MorePhiProcessor;
struct InstanceIdentity;

class MCPToolHandler
{
public:
    static juce::String handle(const juce::String& method,
                               const juce::var& params,
                               MorePhiProcessor& processor,
                               const InstanceIdentity& identity);

    /** Return an MCP-compatible tools/list result object. */
    static juce::String getToolList();

private:
    static juce::String getPluginInfo(MorePhiProcessor& p);
    static juce::String listParameters(const juce::var& params, MorePhiProcessor& p);
    static juce::String getParameter(const juce::var& params, MorePhiProcessor& p);
    static juce::String setParameter(const juce::var& params, MorePhiProcessor& p);
    static juce::String setParametersBatch(const juce::var& params, MorePhiProcessor& p);
    static juce::String captureSnapshot(const juce::var& params, MorePhiProcessor& p);
    static juce::String recallSnapshot(const juce::var& params, MorePhiProcessor& p);
    static juce::String setMorphPosition(const juce::var& params, MorePhiProcessor& p);
    static juce::String getMorphState(MorePhiProcessor& p);

    // ── Ozone mastering tools ────────────────────────────────────────────────

    /** Tool: get_mastering_state — returns LUFS meters, Ozone hosting status, and key parameter values. */
    static juce::String getMasteringState(MorePhiProcessor& p);

    /** Tool: apply_mastering_plan — runs ChainPlanExecutor with provided analysis inputs.
     *  Params: genre_index (int), dynamic_range (float), spectral_tilt (float), correlation_ms (float). */
    static juce::String applyMasteringPlan(const juce::var& params, MorePhiProcessor& p);

    // MCP v1 workflow tools
    static juce::String scanHostedPlugin(const juce::var& params, MorePhiProcessor& p);
    static juce::String loadHostedPlugin(const juce::var& params, MorePhiProcessor& p);
    static juce::String captureHostedState(const juce::var& params, MorePhiProcessor& p);
    static juce::String getAnalysisSummary(MorePhiProcessor& p);
    static juce::String captureAnalysisWindow(const juce::var& params, MorePhiProcessor& p);
    static juce::String compareAnalysis(const juce::var& params, MorePhiProcessor& p);
    static juce::String previewMasteringPlan(const juce::var& params, MorePhiProcessor& p);
    static juce::String renderMasteringBatch(const juce::var& params, MorePhiProcessor& p);
    static juce::String getMasteringRenderStatus(const juce::var& params);
    static juce::String selectMasteringCandidate(const juce::var& params, MorePhiProcessor& p);
    static juce::String auditPluginProfile(MorePhiProcessor& p);
    static juce::String getPluginProfile(const juce::var& params, MorePhiProcessor& p);
    static juce::String savePluginProfile(const juce::var& params, MorePhiProcessor& p);

    // Multi-instance tools
    static juce::String getInstanceInfo(const InstanceIdentity& id);
    static juce::String listInstances();
};

} // namespace more_phi
