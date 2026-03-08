/*
 * MorphSnap — CLI/main.cpp
 * Command-line interface for hosted-plugin dataset smoke tests and renders.
 */

#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Host/PluginHostManager.h"

#include <juce_events/juce_events.h>

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace morphsnap;

namespace {

struct AudioFileInfo
{
    double sampleRate = 48000.0;
    int numChannels = 2;
    juce::int64 lengthInSamples = 0;
};

struct CliOptions
{
    juce::File pluginFile;
    juce::File inputFile;
    juce::File outputDirectory = juce::File::getCurrentWorkingDirectory().getChildFile("output");
    int variations = 100;
    bool listParams = false;
    bool dryRun = false;
    bool verbose = false;
    bool help = false;
    bool hasError = false;
    juce::String errorMessage;
};

void printUsage()
{
    std::cout
        << "MorphSnap Offline Batch Processor v3.3.0\n"
        << "Usage:\n"
        << "  morphsnap-dataset --plugin <path.vst3> --input <dry.wav> [options]\n"
        << "  morphsnap-dataset --plugin <path.vst3> --list-params\n\n"
        << "Options:\n"
        << "  --plugin <path.vst3>   Path to the hosted VST3 plugin\n"
        << "  --input <audio.wav>    Input audio file to process\n"
        << "  --list-params          List hosted plugin parameters and exit\n"
        << "  --dry-run              Validate configuration without rendering audio\n"
        << "  --verbose              Print plugin and render diagnostics\n"
        << "  -o, --output <dir>     Output directory for rendered files\n"
        << "  -n, --variations <N>   Number of rendered variations (default: 100)\n"
        << "  -h, --help             Show this help message\n";
}

CliOptions parseArgs(int argc, char* argv[])
{
    CliOptions options;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String arg(argv[i]);

        if (arg == "-h" || arg == "--help")
        {
            options.help = true;
        }
        else if (arg == "--plugin" && i + 1 < argc)
        {
            options.pluginFile = juce::File(argv[++i]);
        }
        else if (arg == "--input" && i + 1 < argc)
        {
            options.inputFile = juce::File(argv[++i]);
        }
        else if ((arg == "-o" || arg == "--output") && i + 1 < argc)
        {
            options.outputDirectory = juce::File(argv[++i]);
        }
        else if ((arg == "-n" || arg == "--variations") && i + 1 < argc)
        {
            options.variations = juce::String(argv[++i]).getIntValue();
        }
        else if (arg == "--list-params")
        {
            options.listParams = true;
        }
        else if (arg == "--dry-run")
        {
            options.dryRun = true;
        }
        else if (arg == "--verbose")
        {
            options.verbose = true;
        }
        else
        {
            options.hasError = true;
            options.errorMessage = "unknown argument: " + arg;
            return options;
        }
    }

    if (options.help)
        return options;

    if (options.pluginFile == juce::File())
    {
        options.hasError = true;
        options.errorMessage = "--plugin is required.";
        return options;
    }

    if (!options.pluginFile.existsAsFile())
    {
        options.hasError = true;
        options.errorMessage = "plugin file not found: " + options.pluginFile.getFullPathName();
        return options;
    }

    if (!options.listParams && options.inputFile == juce::File())
    {
        options.hasError = true;
        options.errorMessage = "--input is required (unless using --list-params).";
        return options;
    }

    if (!options.listParams && !options.inputFile.existsAsFile())
    {
        options.hasError = true;
        options.errorMessage = "input file not found: " + options.inputFile.getFullPathName();
        return options;
    }

    if (options.variations <= 0)
    {
        options.hasError = true;
        options.errorMessage = "--variations must be greater than zero.";
    }

    return options;
}

bool inspectInputFile(const juce::File& inputFile, AudioFileInfo& info, juce::String& errorMessage)
{
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    auto reader = std::unique_ptr<juce::AudioFormatReader>(formatManager.createReaderFor(inputFile));
    if (!reader)
    {
        errorMessage = "failed to open input audio: " + inputFile.getFullPathName();
        return false;
    }

    info.sampleRate = reader->sampleRate;
    info.numChannels = static_cast<int>(reader->numChannels);
    info.lengthInSamples = reader->lengthInSamples;
    return true;
}

bool discoverPluginDescription(PluginHostManager& hostManager,
                               const juce::File& pluginFile,
                               juce::PluginDescription& description,
                               juce::String& errorMessage)
{
    const auto pluginPath = pluginFile.getFullPathName();

    for (auto* format : hostManager.getFormatManager().getFormats())
    {
        if (!format->fileMightContainThisPluginType(pluginPath))
            continue;

        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, pluginPath);
        if (!descriptions.isEmpty())
        {
            description = *descriptions.getFirst();
            return true;
        }
    }

    description.fileOrIdentifier = pluginPath;
    description.pluginFormatName = pluginFile.hasFileExtension(".vst3") ? "VST3" : juce::String();
    description.name = pluginFile.getFileNameWithoutExtension();
    errorMessage = "failed to discover plugin metadata for: " + pluginPath;
    return false;
}

bool loadPluginForInspection(PluginHostManager& hostManager,
                             const juce::PluginDescription& description,
                             const AudioFileInfo& audioInfo,
                             juce::String& errorMessage)
{
    hostManager.prepare(audioInfo.sampleRate, 512, audioInfo.numChannels);

    if (!hostManager.loadPlugin(description))
    {
        errorMessage = "failed to load plugin: " + description.fileOrIdentifier;
        return false;
    }

    return hostManager.hasPlugin();
}

void printPluginSummary(const juce::PluginDescription& description,
                        const juce::AudioPluginInstance* plugin,
                        bool verbose)
{
    std::cout << "Plugin: " << description.name << "\n";
    std::cout << "Manufacturer: " << description.manufacturerName << "\n";
    std::cout << "Format: " << description.pluginFormatName << "\n";

    if (plugin != nullptr)
    {
        std::cout << "Version: " << description.version << "\n";
        std::cout << "Parameters: " << plugin->getParameters().size() << "\n";

        if (verbose)
        {
            std::cout << "Input channels: " << plugin->getTotalNumInputChannels() << "\n";
            std::cout << "Output channels: " << plugin->getTotalNumOutputChannels() << "\n";
            std::cout << "Latency samples: " << plugin->getLatencySamples() << "\n";
        }
    }
}

void printProfilerSummary(const PerformanceProfiler& profiler)
{
    const auto stats = profiler.getAllStats();
    if (stats.empty())
        return;

    std::cout << "Profiling:\n";
    for (const auto& [name, stat] : stats)
    {
        std::cout << "  " << name
                  << ": calls=" << stat.callCount
                  << ", avgMs=" << stat.averageTimeMs
                  << ", totalMs=" << stat.totalTimeMs << "\n";
    }
}

int runListParams(const CliOptions& options)
{
    PluginHostManager hostManager;
    juce::PluginDescription description;
    juce::String errorMessage;

    discoverPluginDescription(hostManager, options.pluginFile, description, errorMessage);

    AudioFileInfo audioInfo;
    if (options.inputFile.existsAsFile())
    {
        if (!inspectInputFile(options.inputFile, audioInfo, errorMessage))
        {
            std::cerr << "Error: " << errorMessage << "\n";
            return 1;
        }
    }

    if (!loadPluginForInspection(hostManager, description, audioInfo, errorMessage))
    {
        std::cerr << "Error: " << errorMessage << "\n";
        return 1;
    }

    const auto* plugin = hostManager.getPlugin();
    printPluginSummary(description, plugin, options.verbose);

    if (plugin == nullptr)
    {
        std::cerr << "Error: hosted plugin instance is null\n";
        return 1;
    }

    const auto& parameters = plugin->getParameters();
    for (int index = 0; index < parameters.size(); ++index)
    {
        auto* parameter = parameters[index];
        if (parameter == nullptr)
            continue;

        std::cout << std::setw(4) << index << "  "
                  << parameter->getName(256).toStdString() << "\n";
    }

    return 0;
}

int runDryRun(const CliOptions& options)
{
    AudioFileInfo audioInfo;
    juce::String errorMessage;
    if (!inspectInputFile(options.inputFile, audioInfo, errorMessage))
    {
        std::cerr << "Error: " << errorMessage << "\n";
        return 1;
    }

    PluginHostManager hostManager;
    juce::PluginDescription description;
    discoverPluginDescription(hostManager, options.pluginFile, description, errorMessage);

    if (!loadPluginForInspection(hostManager, description, audioInfo, errorMessage))
    {
        std::cerr << "Error: " << errorMessage << "\n";
        return 1;
    }

    if (options.verbose)
    {
        printPluginSummary(description, hostManager.getPlugin(), true);
        std::cout << "Input: " << options.inputFile.getFullPathName() << "\n";
        std::cout << "Output: " << options.outputDirectory.getFullPathName() << "\n";
        std::cout << "Variations: " << options.variations << "\n";
        std::cout << "Sample rate: " << audioInfo.sampleRate << "\n";
        std::cout << "Channels: " << audioInfo.numChannels << "\n";
        std::cout << "Length (samples): " << audioInfo.lengthInSamples << "\n";
    }

    OfflineBatchConfig config;
    config.inputFile = options.inputFile;
    config.outputDirectory = options.outputDirectory;
    config.pluginFile = options.pluginFile;
    config.totalVariations = options.variations;
    config.parallelWorkers = 1;
    config.renderConfig.sampleRate = audioInfo.sampleRate;
    config.renderConfig.blockSize = 512;
    config.renderConfig.numChannels = audioInfo.numChannels;
    config.renderConfig.outputDirectory = options.outputDirectory;

    if (!config.isValid())
    {
        std::cerr << "Error: invalid render configuration\n";
        return 1;
    }

    std::cout << "[Dry Run] Configuration valid.\n";
    return 0;
}

int runRender(const CliOptions& options)
{
    AudioFileInfo audioInfo;
    juce::String errorMessage;
    if (!inspectInputFile(options.inputFile, audioInfo, errorMessage))
    {
        std::cerr << "Error: " << errorMessage << "\n";
        return 1;
    }

    if (!options.outputDirectory.exists() && !options.outputDirectory.createDirectory())
    {
        std::cerr << "Error: failed to create output directory: "
                  << options.outputDirectory.getFullPathName() << "\n";
        return 1;
    }

    OfflineBatchConfig config;
    config.inputFile = options.inputFile;
    config.outputDirectory = options.outputDirectory;
    config.pluginFile = options.pluginFile;
    config.totalVariations = options.variations;
    config.parallelWorkers = 1;
    config.renderConfig.sampleRate = audioInfo.sampleRate;
    config.renderConfig.blockSize = 512;
    config.renderConfig.numChannels = audioInfo.numChannels;
    config.renderConfig.outputDirectory = options.outputDirectory;
    config.renderConfig.validateOutput = true;

    OfflineBatchRenderer renderer;
    if (!renderer.setConfig(config))
    {
        std::cerr << "Error: failed to configure renderer\n";
        return 1;
    }

    juce::CriticalSection completionLock;
    bool renderCompleted = false;
    bool renderSucceeded = false;
    juce::String renderMessage = "Rendering did not complete";

    renderer.onProgressUpdate = [](const OfflineBatchProgress& progress)
    {
        std::cout << "\rProgress: " << progress.completed << "/" << progress.total
                  << " (" << std::fixed << std::setprecision(1) << progress.percentage << "%)"
                  << std::flush;
    };

    renderer.onVariationComplete = [](int index, const RenderResult& result)
    {
        if (!result.success)
        {
            std::cout << "\nVariation " << index << " failed: "
                      << result.errorMessage << "\n";
        }
    };

    renderer.onRenderComplete = [&](bool success, const juce::String& message)
    {
        const juce::ScopedLock lock(completionLock);
        renderCompleted = true;
        renderSucceeded = success;
        renderMessage = message;
    };

    if (options.verbose)
    {
        std::cout << "Input: " << options.inputFile.getFullPathName() << "\n";
        std::cout << "Output: " << options.outputDirectory.getFullPathName() << "\n";
        std::cout << "Variations: " << options.variations << "\n";
        std::cout << "Sample rate: " << audioInfo.sampleRate << "\n";
        std::cout << "Channels: " << audioInfo.numChannels << "\n";
    }

    if (!renderer.startRender())
    {
        std::cerr << "Error: failed to start renderer\n";
        return 1;
    }

    while (renderer.isRendering())
        juce::Thread::sleep(50);

    renderer.stopRender();
    std::cout << "\n";

    {
        const juce::ScopedLock lock(completionLock);
        if (!renderCompleted)
            renderMessage = "Renderer stopped without a completion callback";
    }

    if (options.verbose)
        printProfilerSummary(renderer.getProfiler());

    const auto progress = renderer.getProgress();
    const bool success = renderSucceeded && progress.failedRenders == 0;

    if (!success)
    {
        std::cerr << "Error: " << renderMessage << "\n";
        return 1;
    }

    std::cout << "Rendered " << progress.successfulRenders
              << " variation(s) to " << options.outputDirectory.getFullPathName() << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    const auto options = parseArgs(argc, argv);

    if (options.help)
    {
        printUsage();
        return 0;
    }

    if (options.hasError)
    {
        std::cerr << "Error: " << options.errorMessage << "\n\n";
        printUsage();
        return 1;
    }

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    if (options.listParams)
        return runListParams(options);

    if (options.dryRun)
        return runDryRun(options);

    return runRender(options);
}
