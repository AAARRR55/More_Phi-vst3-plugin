/*
 * MorphSnap — AI/MCPToolHandler.cpp
 * Instance-aware MCP tool dispatch.
 *
 * All JSON responses are constructed with nlohmann::json so that special
 * characters in plugin parameter names (e.g. quotes, backslashes) are
 * correctly escaped without manual handling.
 */
#include "MCPToolHandler.h"
#include "InstanceIdentity.h"
#include "InstanceRegistry.h"
#include "Plugin/PluginProcessor.h"
#include <nlohmann/json.hpp>

namespace morphsnap {

using json = nlohmann::json;

// Serialize a nlohmann::json object to juce::String
static juce::String toJString(const json& j)
{
    return juce::String(j.dump());
}

// Extract a parameter index from a request that may use either "id" or "index" as key.
// Returns -1 if neither key is present.
static int extractParamId(const juce::var& params) noexcept
{
    if (params.hasProperty("id"))    return static_cast<int>(params.getProperty("id",    -1));
    if (params.hasProperty("index")) return static_cast<int>(params.getProperty("index", -1));
    return -1;
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

juce::String MCPToolHandler::handle(const juce::String& method,
                                     const juce::var& params,
                                     MorphSnapProcessor& p,
                                     const InstanceIdentity& identity)
{
    if (method == "get_plugin_info")      return getPluginInfo(p);
    if (method == "list_parameters")      return listParameters(params, p);
    if (method == "get_parameter")        return getParameter(params, p);
    if (method == "set_parameter")        return setParameter(params, p);
    if (method == "set_parameters_batch") return setParametersBatch(params, p);
    if (method == "capture_snapshot")     return captureSnapshot(params, p);
    if (method == "recall_snapshot")      return recallSnapshot(params, p);
    if (method == "set_morph_position")   return setMorphPosition(params, p);
    if (method == "get_morph_state")      return getMorphState(p);

    // Multi-instance tools
    if (method == "get_instance_info")    return getInstanceInfo(identity);
    if (method == "list_instances")       return listInstances();

    return toJString(json{{"error","unknown_method"}});
}

// ── Tool implementations ──────────────────────────────────────────────────────

juce::String MCPToolHandler::getPluginInfo(MorphSnapProcessor& p)
{
    auto* plugin = p.getHostManager().getPlugin();
    if (!plugin)
        return toJString(json{{"name",nullptr},{"type",nullptr},{"paramCount",0}});

    return toJString(json{
        {"name",       plugin->getName().toStdString()},
        {"type",       plugin->acceptsMidi() ? "instrument" : "effect"},
        {"paramCount", static_cast<int>(plugin->getParameters().size())}
    });
}

juce::String MCPToolHandler::listParameters(const juce::var& /*params*/, MorphSnapProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const int count = bridge.getParameterCount();

    json arr = json::array();
    for (int i = 0; i < count; ++i)
    {
        arr.push_back({
            {"id",    i},
            {"name",  bridge.getParameterName(i).toStdString()},
            {"value", bridge.getParameterNormalized(i)}
        });
    }
    return toJString(arr);
}

juce::String MCPToolHandler::getParameter(const juce::var& params, MorphSnapProcessor& p)
{
    const int id = extractParamId(params);
    auto& bridge = p.getParameterBridge();
    if (id < 0 || id >= bridge.getParameterCount())
        return toJString(json{{"error","invalid_param_id"}});

    return toJString(json{
        {"id",    id},
        {"name",  bridge.getParameterName(id).toStdString()},
        {"value", bridge.getParameterNormalized(id)}
    });
}

juce::String MCPToolHandler::setParameter(const juce::var& params, MorphSnapProcessor& p)
{
    const int id = extractParamId(params);
    const float value = static_cast<float>(params.getProperty("value", 0.0));

    auto& bridge = p.getParameterBridge();
    if (id < 0 || id >= bridge.getParameterCount())
        return toJString(json{{"success",false},{"error","invalid_param_id"}});

    if (!p.enqueueParameterSet(id, value))
        return toJString(json{{"success",false},{"error","command_queue_full"}});

    return toJString(json{{"success",true}});
}

juce::String MCPToolHandler::setParametersBatch(const juce::var& params, MorphSnapProcessor& p)
{
    const juce::var batchPayload = params.hasProperty("params")
        ? params.getProperty("params", juce::var())
        : params.getProperty("parameters", juce::var());
    auto* list = batchPayload.getArray();
    if (!list)
        return toJString(json{{"success",false},{"error","missing params/parameters array"}});

    auto& bridge = p.getParameterBridge();
    const int maxParamCount = bridge.getParameterCount();
    int queued = 0;
    int dropped = 0;

    for (const auto& item : *list)
    {
        const int id      = item.getProperty("id",    -1);
        const float value = static_cast<float>(item.getProperty("value", 0.0));
        if (id >= 0 && id < maxParamCount)
        {
            if (p.enqueueParameterSet(id, value))
                ++queued;
            else
                ++dropped;
        }
    }

    return toJString(json{
        {"success", dropped == 0},
        {"queued",  queued},
        {"dropped", dropped}
    });
}

juce::String MCPToolHandler::captureSnapshot(const juce::var& params, MorphSnapProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return toJString(json{{"success",false},{"error","invalid slot"}});

    p.getSnapshotBank().capture(slot, p.getParameterBridge());
    return toJString(json{{"success",true},{"slot",slot}});
}

juce::String MCPToolHandler::recallSnapshot(const juce::var& params, MorphSnapProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return toJString(json{{"success",false},{"error","invalid slot"}});

    if (!p.recallSnapshotQueued(slot))
        return toJString(json{{"success",false},{"error","command_queue_full_or_empty_slot"}});

    return toJString(json{{"success",true},{"slot",slot}});
}

juce::String MCPToolHandler::setMorphPosition(const juce::var& params, MorphSnapProcessor& p)
{
    bool sourceExplicitlySet = false;

    if (params.hasProperty("source"))
    {
        auto source = params.getProperty("source", "").toString().trim().toLowerCase();
        if (source == "xy" || source == "xypad")
        {
            p.setMorphSource(0);
            sourceExplicitlySet = true;
        }
        else if (source == "fader")
        {
            p.setMorphSource(1);
            sourceExplicitlySet = true;
        }
        else
        {
            return toJString(json{{"success",false},{"error","invalid_source"}});
        }
    }

    if (params.hasProperty("x"))
    {
        p.setMorphX(juce::jlimit(0.0f, 1.0f,
                    static_cast<float>(params.getProperty("x", 0.5))));
        if (!sourceExplicitlySet)
            p.setMorphSource(0);
    }

    if (params.hasProperty("y"))
    {
        p.setMorphY(juce::jlimit(0.0f, 1.0f,
                    static_cast<float>(params.getProperty("y", 0.5))));
        if (!sourceExplicitlySet)
            p.setMorphSource(0);
    }

    if (params.hasProperty("fader"))
    {
        p.setFaderPos(juce::jlimit(0.0f, 1.0f,
                      static_cast<float>(params.getProperty("fader", 0.0))));
        p.setMorphSource(1);
    }

    return toJString(json{{"success",true}});
}

juce::String MCPToolHandler::getMorphState(MorphSnapProcessor& p)
{
    return toJString(json{
        {"x",      p.getMorphX()},
        {"y",      p.getMorphY()},
        {"fader",  p.getFaderPos()},
        {"source", p.getMorphSource()}
    });
}

// ── Multi-instance tools ──────────────────────────────────────────────────────

juce::String MCPToolHandler::getInstanceInfo(const InstanceIdentity& id)
{
    return toJString(json{
        {"instanceId",  id.instanceId.toStdString()},
        {"morphCode",   id.morphCode.toStdString()},
        {"port",        id.port},
        {"bearerToken", id.bearerToken.toStdString()},
        {"createdAt",   id.createdAt}
    });
}

juce::String MCPToolHandler::listInstances()
{
    auto instances = InstanceRegistry::getInstance().getAllInstances();

    json arr = json::array();
    for (const auto& inst : instances)
    {
        arr.push_back({
            {"instanceId", inst.instanceId.toStdString()},
            {"morphCode",  inst.morphCode.toStdString()},
            {"port",       inst.port},
            {"createdAt",  inst.createdAt}
            // bearerToken intentionally omitted (redacted)
        });
    }
    return toJString(arr);
}

} // namespace morphsnap
