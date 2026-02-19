/*
 * MorphSnap — AI/MCPToolHandler.h
 * Dispatches MCP tool calls to plugin subsystems.
 * Instance-aware: includes identity context in responses.
 */
#pragma once

#include <juce_core/juce_core.h>

namespace morphsnap {

class MorphSnapProcessor;
struct InstanceIdentity;

class MCPToolHandler
{
public:
    static juce::String handle(const juce::String& method,
                               const juce::var& params,
                               MorphSnapProcessor& processor,
                               const InstanceIdentity& identity);

private:
    static juce::String getPluginInfo(MorphSnapProcessor& p);
    static juce::String listParameters(const juce::var& params, MorphSnapProcessor& p);
    static juce::String getParameter(const juce::var& params, MorphSnapProcessor& p);
    static juce::String setParameter(const juce::var& params, MorphSnapProcessor& p);
    static juce::String setParametersBatch(const juce::var& params, MorphSnapProcessor& p);
    static juce::String captureSnapshot(const juce::var& params, MorphSnapProcessor& p);
    static juce::String recallSnapshot(const juce::var& params, MorphSnapProcessor& p);
    static juce::String setMorphPosition(const juce::var& params, MorphSnapProcessor& p);
    static juce::String getMorphState(MorphSnapProcessor& p);

    // Multi-instance tools
    static juce::String getInstanceInfo(const InstanceIdentity& id);
    static juce::String listInstances();
};

} // namespace morphsnap
