#include "OzonePluginBackend.h"

#include "Host/IPluginHostManager.h"
#include "Host/ParameterBridge.h"
#include "Host/PluginHostManager.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace more_phi::standalone_mcp {

using json = nlohmann::json;

namespace {

juce::String envOrDefault(const char* key, const char* fallback = "")
{
    return juce::SystemStats::getEnvironmentVariable(key, fallback);
}

int envInt(const char* key, int fallback, int minValue, int maxValue)
{
    const auto value = envOrDefault(key);
    if (value.isEmpty())
        return fallback;

    return juce::jlimit(minValue, maxValue, value.getIntValue());
}

double envDouble(const char* key, double fallback, double minValue, double maxValue)
{
    const auto value = envOrDefault(key);
    if (value.isEmpty())
        return fallback;

    return juce::jlimit(minValue, maxValue, value.getDoubleValue());
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
    {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsIgnoreCase(const juce::String& value, const std::string& query)
{
    if (query.empty())
        return true;

    return value.toLowerCase().contains(juce::String(query).toLowerCase());
}

ToolCallOutcome makeToolError(std::string error, std::string message = {})
{
    json body{{"success", false}, {"error", std::move(error)}};
    if (!message.empty())
        body["message"] = std::move(message);
    return {body, true};
}

json makeApplyError(std::string code, int index, std::string message)
{
    return {
        {"code", std::move(code)},
        {"index", index},
        {"message", std::move(message)}
    };
}

juce::AudioPluginInstance* acquirePlugin(IPluginHostManager& host,
                                         PluginHostManager*& concreteHost,
                                         bool& exclusive)
{
    concreteHost = dynamic_cast<PluginHostManager*>(&host);
    exclusive = false;

    if (concreteHost != nullptr)
    {
        if (auto* plugin = concreteHost->beginExclusivePluginUse(500))
        {
            exclusive = true;
            return plugin;
        }
        return nullptr;
    }

    return host.getPlugin();
}

void releasePlugin(PluginHostManager* concreteHost, bool exclusive)
{
    if (exclusive && concreteHost != nullptr)
        concreteHost->endExclusivePluginUse();
}

} // namespace

HostedOzonePluginBackend::HostedOzonePluginBackend()
{
    ownedHost = std::make_unique<PluginHostManager>();
    host = ownedHost.get();
    bridge = std::make_unique<ParameterBridge>(*host);

    sampleRate = envDouble("OZONE_SAMPLE_RATE", 44100.0, 8000.0, 384000.0);
    blockSize = envInt("OZONE_BLOCK_SIZE", 512, 16, 8192);
    numChannels = envInt("OZONE_NUM_CHANNELS", 2, 1, 16);

    loadFromEnvironment();
}

HostedOzonePluginBackend::HostedOzonePluginBackend(IPluginHostManager& externalHost)
    : host(&externalHost),
      bridge(std::make_unique<ParameterBridge>(externalHost))
{
}

HostedOzonePluginBackend::~HostedOzonePluginBackend()
{
    if (ownedHost != nullptr)
        ownedHost->unloadPlugin();
}

void HostedOzonePluginBackend::loadFromEnvironment()
{
    jassert(host != nullptr);

    const auto ozonePath = envOrDefault("OZONE_VST3_PATH");
    if (ozonePath.isEmpty())
    {
        loadError = "OZONE_VST3_PATH is required for the standalone Ozone MCP server.";
        return;
    }

    const juce::File pluginFile(ozonePath);
    if (!pluginFile.exists())
    {
        loadError = "OZONE_VST3_PATH does not exist: " + ozonePath;
        return;
    }

    host->prepare(sampleRate, blockSize, numChannels);

    juce::PluginDescription description;
    juce::String discoveryDetails;
    const bool verbose = envOrDefault("OZONE_DISCOVERY_VERBOSE").getIntValue() != 0;
    const bool discovered = PluginHostManager::discoverPlugin(
        host->getFormatManager(), pluginFile, description, discoveryDetails, verbose);

    if (!discovered && description.fileOrIdentifier.isEmpty())
    {
        loadError = discoveryDetails.isNotEmpty()
            ? discoveryDetails
            : "Could not discover Ozone plugin at: " + ozonePath;
        return;
    }

    if (!host->loadPlugin(description))
    {
        loadError = "Failed to load Ozone plugin from " + ozonePath;
        if (discoveryDetails.isNotEmpty())
            loadError += " (" + discoveryDetails + ")";
        return;
    }

    loadError.clear();
}

bool HostedOzonePluginBackend::hasLoadedPlugin() const
{
    return host != nullptr && host->hasPlugin() && host->getPlugin() != nullptr;
}

ToolCallOutcome HostedOzonePluginBackend::pluginNotLoaded() const
{
    return makeToolError(
        "plugin_not_loaded",
        loadError.isNotEmpty()
            ? loadError.toStdString()
            : "No Ozone plugin instance is loaded.");
}

json HostedOzonePluginBackend::pluginInfo() const
{
    json info = {
        {"loaded", hasLoadedPlugin()},
        {"sample_rate", sampleRate},
        {"block_size", blockSize},
        {"channels", numChannels}
    };

    if (loadError.isNotEmpty())
        info["load_error"] = loadError.toStdString();

    if (host == nullptr)
        return info;

    if (const auto* desc = host->getLastDescription())
    {
        info["name"] = desc->name.toStdString();
        info["descriptive_name"] = desc->descriptiveName.toStdString();
        info["manufacturer"] = desc->manufacturerName.toStdString();
        info["format"] = desc->pluginFormatName.toStdString();
        info["identifier"] = desc->fileOrIdentifier.toStdString();
    }
    else if (const auto* plugin = host->getPlugin())
    {
        info["name"] = plugin->getName().toStdString();
    }

    return info;
}

json HostedOzonePluginBackend::parameterDescriptorToJson(int index, bool includeValues) const
{
    const auto descriptor = bridge->getParameterDescriptor(index);
    if (descriptor.index < 0)
        return json::object();

    json result{
        {"index", descriptor.index},
        {"stable_id", descriptor.stableId.toStdString()},
        {"name", descriptor.name.toStdString()},
        {"label", descriptor.label.toStdString()},
        {"num_steps", descriptor.numSteps},
        {"default_value", descriptor.defaultValue},
        {"is_discrete", descriptor.discrete},
        {"is_boolean", descriptor.boolean}
    };

    if (includeValues)
    {
        result["value"] = descriptor.value;
        result["text"] = descriptor.displayValue.toStdString();
    }

    return result;
}

ToolCallOutcome HostedOzonePluginBackend::getParameters(const ParameterListArgs& args)
{
    if (!hasLoadedPlugin())
        return pluginNotLoaded();

    const auto query = args.query ? lower(*args.query) : std::string();
    const auto descriptors = bridge->getParameterDescriptors();
    json parameters = json::array();

    for (const auto& descriptor : descriptors)
    {
        if (!query.empty()
            && !containsIgnoreCase(descriptor.name, query)
            && !containsIgnoreCase(descriptor.stableId, query))
            continue;

        json item{
            {"index", descriptor.index},
            {"stable_id", descriptor.stableId.toStdString()},
            {"name", descriptor.name.toStdString()},
            {"label", descriptor.label.toStdString()},
            {"num_steps", descriptor.numSteps},
            {"default_value", descriptor.defaultValue},
            {"is_discrete", descriptor.discrete},
            {"is_boolean", descriptor.boolean}
        };

        if (args.includeValues)
        {
            item["value"] = descriptor.value;
            item["text"] = descriptor.displayValue.toStdString();
        }

        parameters.push_back(std::move(item));
    }

    return {json{
        {"success", true},
        {"plugin", pluginInfo()},
        {"count", static_cast<int>(descriptors.size())},
        {"returned", static_cast<int>(parameters.size())},
        {"parameters", parameters}
    }, false};
}

ToolCallOutcome HostedOzonePluginBackend::setParameter(int index, float value)
{
    if (!hasLoadedPlugin())
        return pluginNotLoaded();

    const int count = bridge->getParameterCount();
    if (index < 0 || index >= count)
        return makeToolError("invalid_parameter_index",
                             "Parameter index is outside the hosted plugin parameter range.");

    bridge->setParameterNormalized(index, juce::jlimit(0.0f, 1.0f, value));

    return {json{
        {"success", true},
        {"parameter", parameterDescriptorToJson(index, true)}
    }, false};
}

ToolCallOutcome HostedOzonePluginBackend::applyAssistantParameters(const AssistantParameterApplyArgs& args)
{
    if (!hasLoadedPlugin())
        return pluginNotLoaded();

    const int count = bridge->getParameterCount();
    json errors = json::array();

    for (const auto& decision : args.parameters)
    {
        if (decision.index < 0 || decision.index >= count)
        {
            errors.push_back(makeApplyError(
                "parameter_index_out_of_range",
                decision.index,
                "Assistant parameter index is outside the hosted plugin parameter range."));
            continue;
        }

        if (!std::isfinite(decision.value) || decision.value < 0.0 || decision.value > 1.0)
        {
            errors.push_back(makeApplyError(
                "parameter_value_out_of_range",
                decision.index,
                "Assistant parameter value must be finite and normalized between 0.0 and 1.0."));
        }
    }

    if (!errors.empty())
    {
        return {json{
            {"success", false},
            {"error", "assistant_apply_failed"},
            {"apply_result", {
                {"applied", false},
                {"requested_count", static_cast<int>(args.parameters.size())},
                {"applied_count", 0},
                {"parameters", json::array()},
                {"errors", errors}
            }}
        }, true};
    }

    json applied = json::array();
    for (const auto& decision : args.parameters)
    {
        bridge->setParameterNormalized(decision.index, static_cast<float>(decision.value));
        applied.push_back({
            {"index", decision.index},
            {"value", decision.value},
            {"success", true},
            {"parameter", parameterDescriptorToJson(decision.index, true)}
        });
    }

    return {json{
        {"success", true},
        {"apply_result", {
            {"applied", true},
            {"requested_count", static_cast<int>(args.parameters.size())},
            {"applied_count", static_cast<int>(applied.size())},
            {"parameters", applied},
            {"errors", json::array()}
        }}
    }, false};
}

std::optional<int> HostedOzonePluginBackend::resolveAssistantParameter(const RunAssistantArgs& args) const
{
    if (args.assistantParameterIndex)
        return args.assistantParameterIndex;

    const auto envIndex = envOrDefault("OZONE_ASSISTANT_PARAM_INDEX");
    if (envIndex.isNotEmpty())
        return envIndex.getIntValue();

    const auto descriptors = bridge->getParameterDescriptors();
    for (const auto& descriptor : descriptors)
    {
        const auto name = descriptor.name.toLowerCase();
        const auto stableId = descriptor.stableId.toLowerCase();
        if (name.contains("assistant")
            || name.contains("analyze")
            || stableId.contains("assistant")
            || stableId.contains("analyze"))
            return descriptor.index;
    }

    return std::nullopt;
}

ToolCallOutcome HostedOzonePluginBackend::renderInputAudio(const std::string& inputAudioPath,
                                                           double analysisSeconds,
                                                           int& renderedSamples)
{
    renderedSamples = 0;

    const juce::File inputFile{juce::String(inputAudioPath)};
    if (!inputFile.existsAsFile())
        return makeToolError("input_audio_not_found", "input_audio_path does not point to a readable file.");

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(inputFile));
    if (reader == nullptr)
        return makeToolError("unsupported_audio_file", "Could not create an audio reader for input_audio_path.");

    const int channels = juce::jlimit(1, 16, static_cast<int>(reader->numChannels));
    const auto maxSamplesDouble = std::max(1.0, analysisSeconds) * reader->sampleRate;
    const auto maxSamples = static_cast<juce::int64>(
        std::min<double>(maxSamplesDouble, static_cast<double>(std::numeric_limits<int>::max())));
    const auto totalSamples = std::min<juce::int64>(reader->lengthInSamples, maxSamples);

    juce::AudioBuffer<float> buffer(channels, blockSize);
    juce::MidiBuffer midi;

    for (juce::int64 position = 0; position < totalSamples; position += blockSize)
    {
        const int samplesThisBlock = static_cast<int>(
            std::min<juce::int64>(blockSize, totalSamples - position));

        buffer.clear();
        if (!reader->read(&buffer, 0, samplesThisBlock, position, true, true))
            return makeToolError("audio_read_failed", "Failed while reading input_audio_path.");

        host->processBlock(buffer, midi);
        renderedSamples += samplesThisBlock;
        midi.clear();
    }

    return {json{{"success", true}, {"rendered_samples", renderedSamples}}, false};
}

ToolCallOutcome HostedOzonePluginBackend::runMasterAssistant(const RunAssistantArgs& args)
{
    if (!hasLoadedPlugin())
        return pluginNotLoaded();

    const auto maybeIndex = resolveAssistantParameter(args);
    if (!maybeIndex)
    {
        return makeToolError(
            "assistant_parameter_not_found",
            "No automatable Ozone parameter containing 'assistant' or 'analyze' was found. "
            "Set OZONE_ASSISTANT_PARAM_INDEX or pass assistant_parameter_index if this Ozone version exposes one.");
    }

    const int index = *maybeIndex;
    const int count = bridge->getParameterCount();
    if (index < 0 || index >= count)
        return makeToolError("invalid_assistant_parameter_index");

    int renderedSamples = 0;
    bridge->setParameterNormalized(index, 1.0f);

    ToolCallOutcome renderOutcome{{"success", true}, false};
    if (args.inputAudioPath && !args.inputAudioPath->empty())
        renderOutcome = renderInputAudio(*args.inputAudioPath, args.analysisSeconds, renderedSamples);
    else
        juce::Thread::sleep(100);

    bridge->setParameterNormalized(index, 0.0f);

    if (renderOutcome.isError)
        return renderOutcome;

    return {json{
        {"success", true},
        {"assistant_parameter_index", index},
        {"assistant_parameter", parameterDescriptorToJson(index, true)},
        {"input_audio_path", args.inputAudioPath.value_or("")},
        {"rendered_samples", renderedSamples}
    }, false};
}

ToolCallOutcome HostedOzonePluginBackend::getState()
{
    if (!hasLoadedPlugin())
        return pluginNotLoaded();

    PluginHostManager* concreteHost = nullptr;
    bool exclusive = false;
    auto* plugin = acquirePlugin(*host, concreteHost, exclusive);
    if (plugin == nullptr)
        return makeToolError("plugin_busy", "Could not acquire Ozone for state capture.");

    juce::MemoryBlock state;
    try
    {
        plugin->getStateInformation(state);
    }
    catch (...)
    {
        releasePlugin(concreteHost, exclusive);
        return makeToolError("state_capture_failed");
    }

    releasePlugin(concreteHost, exclusive);

    return {json{
        {"success", true},
        {"state_base64", juce::Base64::toBase64(state.getData(), state.getSize()).toStdString()},
        {"size_bytes", static_cast<int>(state.getSize())}
    }, false};
}

ToolCallOutcome HostedOzonePluginBackend::setState(const std::string& stateBase64)
{
    if (!hasLoadedPlugin())
        return pluginNotLoaded();

    juce::MemoryBlock state;
    {
        juce::MemoryOutputStream decoded(state, false);
        if (!juce::Base64::convertFromBase64(decoded, juce::String(stateBase64)))
            return makeToolError("invalid_state_base64");
    }

    PluginHostManager* concreteHost = nullptr;
    bool exclusive = false;
    auto* plugin = acquirePlugin(*host, concreteHost, exclusive);
    if (plugin == nullptr)
        return makeToolError("plugin_busy", "Could not acquire Ozone for state restore.");

    try
    {
        plugin->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }
    catch (...)
    {
        releasePlugin(concreteHost, exclusive);
        return makeToolError("state_restore_failed");
    }

    releasePlugin(concreteHost, exclusive);

    return {json{
        {"success", true},
        {"size_bytes", static_cast<int>(state.getSize())}
    }, false};
}

std::unique_ptr<OzonePluginBackend> createOzonePluginBackend()
{
    return std::make_unique<HostedOzonePluginBackend>();
}

} // namespace more_phi::standalone_mcp
