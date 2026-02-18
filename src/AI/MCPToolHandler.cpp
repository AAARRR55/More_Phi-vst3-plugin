/*
 * MorphSnap — AI/MCPToolHandler.cpp
 */
#include "MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"

namespace morphsnap {

juce::String MCPToolHandler::handle(const juce::String& method,
                                     const juce::var& params,
                                     MorphSnapProcessor& p)
{
    if (method == "get_plugin_info")     return getPluginInfo(p);
    if (method == "list_parameters")     return listParameters(params, p);
    if (method == "get_parameter")       return getParameter(params, p);
    if (method == "set_parameter")       return setParameter(params, p);
    if (method == "set_parameters_batch") return setParametersBatch(params, p);
    if (method == "capture_snapshot")    return captureSnapshot(params, p);
    if (method == "recall_snapshot")     return recallSnapshot(params, p);
    if (method == "set_morph_position")  return setMorphPosition(params, p);
    if (method == "get_morph_state")     return getMorphState(p);

    return R"({"error":"unknown_method"})";
}

juce::String MCPToolHandler::getPluginInfo(MorphSnapProcessor& p)
{
    auto* plugin = p.getHostManager().getPlugin();
    if (!plugin) return R"({"name":null,"type":null,"paramCount":0})";

    return "{\"name\":" + juce::JSON::toString(plugin->getName()) +
           ",\"type\":\"" + (plugin->acceptsMidi() ? "instrument" : "effect") +
           "\",\"paramCount\":" + juce::String(plugin->getParameters().size()) + "}";
}

juce::String MCPToolHandler::listParameters(const juce::var& /*params*/, MorphSnapProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const int count = bridge.getParameterCount();

    juce::String json = "[";
    for (int i = 0; i < count; ++i)
    {
        if (i > 0) json += ",";
        json += "{\"id\":" + juce::String(i) +
                ",\"name\":" + juce::JSON::toString(bridge.getParameterName(i)) +
                ",\"value\":" + juce::String(bridge.getParameterNormalized(i), 4) + "}";
    }
    json += "]";
    return json;
}

juce::String MCPToolHandler::getParameter(const juce::var& params, MorphSnapProcessor& p)
{
    int id = params.getProperty("id", -1);
    auto& bridge = p.getParameterBridge();
    if (id < 0 || id >= bridge.getParameterCount())
        return R"({"error":"invalid_param_id"})";

    return "{\"id\":" + juce::String(id) +
           ",\"name\":" + juce::JSON::toString(bridge.getParameterName(id)) +
           ",\"value\":" + juce::String(bridge.getParameterNormalized(id), 4) + "}";
}

juce::String MCPToolHandler::setParameter(const juce::var& params, MorphSnapProcessor& p)
{
    int id = params.getProperty("id", -1);
    float value = static_cast<float>(params.getProperty("value", 0.0));

    // Push to lock-free command queue (consumed by audio thread)
    MorphSnapProcessor::ParamCommand cmd{id, value};
    p.commandQueue.push(cmd);

    return R"({"success":true})";
}

juce::String MCPToolHandler::setParametersBatch(const juce::var& params, MorphSnapProcessor& p)
{
    auto* list = params.getProperty("params", juce::var()).getArray();
    if (!list) return R"({"success":false,"error":"missing params array"})";

    int count = 0;
    for (const auto& item : *list)
    {
        int id = item.getProperty("id", -1);
        float value = static_cast<float>(item.getProperty("value", 0.0));
        MorphSnapProcessor::ParamCommand cmd{id, value};
        if (p.commandQueue.push(cmd)) ++count;
    }

    return "{\"success\":true,\"count\":" + juce::String(count) + "}";
}

juce::String MCPToolHandler::captureSnapshot(const juce::var& params, MorphSnapProcessor& p)
{
    int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return R"({"success":false,"error":"invalid slot"})";

    p.getSnapshotBank().capture(slot, p.getParameterBridge());
    return "{\"success\":true,\"slot\":" + juce::String(slot) + "}";
}

juce::String MCPToolHandler::recallSnapshot(const juce::var& params, MorphSnapProcessor& p)
{
    int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return R"({"success":false,"error":"invalid slot"})";

    p.getSnapshotBank().recall(slot, p.getParameterBridge());
    return "{\"success\":true}";
}

juce::String MCPToolHandler::setMorphPosition(const juce::var& params, MorphSnapProcessor& p)
{
    if (params.hasProperty("x"))
        p.morphX.store(static_cast<float>(params.getProperty("x", 0.5)),
                       std::memory_order_relaxed);
    if (params.hasProperty("y"))
        p.morphY.store(static_cast<float>(params.getProperty("y", 0.5)),
                       std::memory_order_relaxed);
    if (params.hasProperty("fader"))
        p.faderPos.store(static_cast<float>(params.getProperty("fader", 0.0)),
                         std::memory_order_relaxed);

    return R"({"success":true})";
}

juce::String MCPToolHandler::getMorphState(MorphSnapProcessor& p)
{
    return "{\"x\":" + juce::String(p.morphX.load(), 4) +
           ",\"y\":" + juce::String(p.morphY.load(), 4) +
           ",\"fader\":" + juce::String(p.faderPos.load(), 4) +
           ",\"source\":" + juce::String(p.morphSource.load()) + "}";
}

} // namespace morphsnap
