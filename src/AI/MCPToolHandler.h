/*
 * MorphSnap — AI/MCPToolHandler.h
 * Dispatches MCP tool calls to plugin subsystems.
 */
#pragma once

#include <juce_core/juce_core.h>

namespace morphsnap {

class MorphSnapProcessor;

class MCPToolHandler
{
public:
    static juce::String handle(const juce::String& method,
                               const juce::var& params,
                               MorphSnapProcessor& processor);

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
};

} // namespace morphsnap
