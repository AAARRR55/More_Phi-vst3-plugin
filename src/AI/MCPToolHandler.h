/*
 * More-Phi — AI/MCPToolHandler.h
 * Dispatches MCP tool calls to plugin subsystems.
 * Instance-aware: includes identity context in responses.
 */
#pragma once

#include <juce_core/juce_core.h>
#include "ToolResultCache.h"
#include "AsyncToolExecutor.h"

namespace more_phi {

class MorePhiProcessor;
struct InstanceIdentity;
class AutomationRuntime;

class MCPToolHandler
{
public:
    static juce::String handle(const juce::String& method,
                               const juce::var& params,
                               MorePhiProcessor& processor,
                               const InstanceIdentity& identity,
                               AutomationRuntime& runtime);

    /** Convenience overload for callers without a runtime reference.
     *  Creates a temporary runtime for read-only operations. */
    static juce::String handle(const juce::String& method,
                               const juce::var& params,
                               MorePhiProcessor& processor,
                               const InstanceIdentity& identity);

    /** Return an MCP-compatible tools/list result object. */
    static juce::String getToolList();

    /** Invalidate cached read-only tool results, e.g. after plugin load. */
    static void invalidateToolResultCache();

    /** Scoped invalidation after a successful write transaction (spec §6.2).
     *  Maps @p toolName to the cache scopes it actually dirties (e.g. a
     *  set_parameter evicts parameter-describing reads but preserves analysis
     *  meters and the semantic profile). Falls back to a conservative
     *  parameter+morph+analysis eviction for unrecognised write tools. */
    static void invalidateToolResultCacheForTool(const juce::String& toolName);

    /** Access the async executor for long-running tools. */
    static AsyncToolExecutor& getAsyncToolExecutor();

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
    static juce::String runSelfTest(const juce::var& params, MorePhiProcessor& p);
    static juce::String listMorePhiParameters(MorePhiProcessor& p);
    static juce::String getMorePhiParameter(const juce::var& params, MorePhiProcessor& p);
    static juce::String setMorePhiParameter(const juce::var& params, MorePhiProcessor& p);
    static juce::String setMorePhiParameters(const juce::var& params, MorePhiProcessor& p);

    // ── Ozone mastering tools ────────────────────────────────────────────────

    /** Tool: ozone.audit_parameters — discover Ozone parameter indices from the hosted plugin. */
    static juce::String auditOzoneParameters(const juce::var& params, MorePhiProcessor& p);

    /** Tool: get_mastering_state — returns LUFS meters, Ozone hosting status, and key parameter values. */
    static juce::String getMasteringState(MorePhiProcessor& p);

    /** Tool: apply_mastering_plan — runs the heuristic ChainPlanExecutor with provided analysis inputs.
     *  Params: genre_index (int), dynamic_range (float), spectral_tilt (float), correlation_ms (float). */
    static juce::String applyMasteringPlan(const juce::var& params, MorePhiProcessor& p);

    // ── iZotope IPC Assistant tools ─────────────────────────────────────────
    static juce::String izotopeIpcAttach(const juce::var& params, MorePhiProcessor& p);
    static juce::String izotopeIpcDetach(MorePhiProcessor& p);
    static juce::String izotopeIpcStatus(MorePhiProcessor& p);
    static juce::String izotopeIpcSnapshot(const juce::var& params, MorePhiProcessor& p);
    static juce::String izotopeIpcDump(const juce::var& params, MorePhiProcessor& p);
    static juce::String izotopeIpcCapture(const juce::var& params, MorePhiProcessor& p);
    static juce::String ozoneRunAssistantIpc(const juce::var& params, MorePhiProcessor& p);

    // ── Ozone Track Assistant tools (guide-aligned) ──────────────────────────

    /** Tool: ozone.track.get_info — session metadata, mastering status, current LUFS. */
    static juce::String ozoneTrackGetInfo(const juce::var& params, MorePhiProcessor& p,
                                          const InstanceIdentity& identity);

    /** Tool: ozone.track.update_status — transition mastering workflow state.
     *  Valid transitions enforced; reason required for rejected/on_hold. */
    static juce::String ozoneTrackUpdateStatus(const juce::var& params,
                                               const InstanceIdentity& identity);

    /** Tool: ozone.track.analyze — return LUFS/dBTP/DR metrics with streaming-target
     *  comparison and heuristic mastering plan recommendation. */
    static juce::String ozoneTrackAnalyze(const juce::var& params, MorePhiProcessor& p,
                                          const InstanceIdentity& identity);

    /** Tool: ozone.track.search — search loaded snapshots/sessions by title or status.
     *  Returns paginated list compatible with the Track Assistant JSON schema. */
    static juce::String ozoneTrackSearch(const juce::var& params, MorePhiProcessor& p,
                                         const InstanceIdentity& identity);

    // MCP v1 workflow tools
    static juce::String diagnoseParameterPipeline(const juce::var& params, MorePhiProcessor& p);
    static juce::String scanHostedPlugin(const juce::var& params, MorePhiProcessor& p);
    static juce::String loadHostedPlugin(const juce::var& params, MorePhiProcessor& p);
    static juce::String captureHostedState(const juce::var& params, MorePhiProcessor& p);
    static juce::String getAnalysisSummary(MorePhiProcessor& p);
    static juce::String getSpectrumAnalysis(const juce::var& params, MorePhiProcessor& p);
    static juce::String getStereoFieldAnalysis(MorePhiProcessor& p);
    static juce::String captureAnalysisWindow(const juce::var& params, MorePhiProcessor& p);
    static juce::String compareAnalysis(const juce::var& params, MorePhiProcessor& p);
    static juce::String previewMasteringPlan(const juce::var& params, MorePhiProcessor& p);
    static juce::String renderMasteringBatch(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity);
    static juce::String getMasteringRenderStatus(const juce::var& params, const juce::String& instanceId);
    static juce::String selectMasteringCandidate(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity);
    static juce::String auditPluginProfile(MorePhiProcessor& p);
    static juce::String describePluginSemantics(MorePhiProcessor& p);
    static juce::String applySafePluginAction(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity);
    static juce::String restoreSafePluginSnapshot(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity);
    static juce::String getPluginProfile(const juce::var& params, MorePhiProcessor& p);
    static juce::String savePluginProfile(const juce::var& params, MorePhiProcessor& p);
    static juce::String describePluginSemanticMap(const juce::var& params, MorePhiProcessor& p);

    // Multi-instance tools
    static juce::String getInstanceInfo(const InstanceIdentity& id);
    static juce::String listInstances();

    // Async tool management
    static juce::String submitAsyncTool(const juce::String& method,
                                        const juce::var& params,
                                        MorePhiProcessor& p,
                                        const InstanceIdentity& identity,
                                        AutomationRuntime& runtime);
    static juce::String getAsyncToolStatus(const juce::var& params);
    static juce::String getAsyncToolResult(const juce::var& params);

    // Caching helpers
    static bool isCacheableTool(const juce::String& method);
    static juce::String getCachedToolResult(const juce::String& method,
                                            const juce::var& params,
                                            MorePhiProcessor& p);
    static void cacheToolResult(const juce::String& method,
                                const juce::var& params,
                                MorePhiProcessor& p,
                                const juce::String& result);

    static ToolResultCache& getToolResultCache();
    static AsyncToolExecutor& getAsyncToolExecutorInternal();
};

} // namespace more_phi
