/*
 * More-Phi — CLI/main.cpp
 * Command-line interface for hosted-plugin dataset smoke tests and renders.
 */

#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Host/PluginHostManager.h"

#include <juce_events/juce_events.h>

#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace more_phi;

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
    juce::File configDirectory;
    juce::File outputDirectory = juce::File::getCurrentWorkingDirectory().getChildFile("output");
    int variations = 100;
    int workers = 1;
    bool enableSimd = true;
    bool usePool = true;
    double durationSeconds = 0.0;  // 0.0 means use full input duration
    bool listParams = false;
    bool dryRun = false;
    bool verbose = false;
    bool help = false;
    bool version = false;
    bool hasError = false;
    juce::String errorMessage;

    static constexpr int MAX_VARIATIONS = 10000;  // Prevent DoS via excessive variations
};

static constexpr const char* kVersion = "v3.3.0";
static constexpr int kDefaultBlockSize = 512;
static constexpr int kFallbackParamCount = 100;  // Safe default when plugin param count is unknown

void printUsage()
{
    std::cout
        << "MorePhi Offline Batch Processor " << kVersion << "\n"
        << "Usage:\n"
        << "  morephi-dataset --plugin <path.vst3> --input <dry.wav> [options]\n"
        << "  morephi-dataset --plugin <path.vst3> --list-params\n\n"
        << "Options:\n"
        << "  --plugin <path.vst3>   Path to the hosted VST3 plugin\n"
        << "  --input <audio.wav>    Input audio file to process\n"
        << "  --duration <seconds>   Maximum duration to render in seconds (default: 0 = full file)\n"
        << "  --list-params          List hosted plugin parameters and exit\n"
        << "  --config-dir <dir>     Directory containing JSON parameter configs to render\n"
        << "  --dry-run              Validate configuration without rendering audio\n"
        << "  --verbose              Print plugin and render diagnostics\n"
        << "  -o, --output <dir>     Output directory for rendered files\n"
        << "  -n, --variations <N>   Number of rendered variations (default: 100)\n"
        << "  -j, --workers <N>      Number of parallel worker threads (default: 1)\n"
        << "  --no-simd              Disable SIMD-accelerated processing\n"
        << "  --no-pool              Disable memory pool for audio buffers\n"
        << "  --version              Print version and exit\n"
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
        else if (arg == "--plugin")
        {
            if (i + 1 < argc) options.pluginFile = juce::File(argv[++i]);
            else { options.hasError = true; options.errorMessage = "--plugin requires a file path"; return options; }
        }
        else if (arg == "--input")
        {
            if (i + 1 < argc) options.inputFile = juce::File(argv[++i]);
            else { options.hasError = true; options.errorMessage = "--input requires a file path"; return options; }
        }
        else if (arg == "-o" || arg == "--output")
        {
            if (i + 1 < argc) options.outputDirectory = juce::File(argv[++i]);
            else { options.hasError = true; options.errorMessage = "--output requires a directory path"; return options; }
        }
        else if (arg == "-n" || arg == "--variations")
        {
            if (i + 1 < argc) options.variations = juce::String(argv[++i]).getIntValue();
            else { options.hasError = true; options.errorMessage = "--variations requires a number"; return options; }
        }
        else if (arg == "--duration")
        {
            if (i + 1 < argc) options.durationSeconds = juce::String(argv[++i]).getDoubleValue();
            else { options.hasError = true; options.errorMessage = "--duration requires a number"; return options; }
        }
        else if (arg == "--list-params")
        {
            options.listParams = true;
        }
        else if (arg == "--dry-run")
        {
            options.dryRun = true;
        }
        else if (arg == "-v" || arg == "--verbose")
        {
            options.verbose = true;
        }
        else if (arg == "-j" || arg == "--workers")
        {
            if (i + 1 < argc) options.workers = juce::String(argv[++i]).getIntValue();
            else { options.hasError = true; options.errorMessage = "--workers requires a number"; return options; }
        }
        else if (arg == "--no-simd")
        {
            options.enableSimd = false;
        }
        else if (arg == "--no-pool")
        {
            options.usePool = false;
        }
        else if (arg == "--version")
        {
            options.version = true;
        }
        else if (arg == "--config-dir")
        {
            if (i + 1 < argc) options.configDirectory = juce::File(argv[++i]);
            else { options.hasError = true; options.errorMessage = "--config-dir requires a directory path"; return options; }
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

    if (!options.pluginFile.existsAsFile() && !options.pluginFile.isDirectory())
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
        return options;
    }

    if (options.durationSeconds < 0.0)
    {
        options.hasError = true;
        options.errorMessage = "--duration cannot be negative.";
        return options;
    }

    // Security: Prevent DoS via excessive variations
    if (options.variations > CliOptions::MAX_VARIATIONS)
    {
        options.hasError = true;
        options.errorMessage = "--variations exceeds maximum limit of " +
                               juce::String(CliOptions::MAX_VARIATIONS) + ".";
        return options;
    }

    // Security: Prevent writing to system directories
    {
        // M1: Resolve symlinks before comparison to prevent bypass via
        // symlinked paths (e.g., ./safe_dir → C:\Windows\System32)
        const auto resolvedOutput = options.outputDirectory.isSymbolicLink()
            ? options.outputDirectory.getLinkedTarget()
            : options.outputDirectory;
        const auto outputPath = resolvedOutput.getFullPathName();
        const juce::StringArray protectedPaths = {
            "/", "/usr", "/bin", "/sbin", "/etc", "/root", "/var", "/lib",
            "/boot", "/dev", "/proc", "/sys", "/run", "/opt", "/srv",
            "C:\\", "C:\\Windows", "C:\\Program Files", "C:\\Program Files (x86)",
            "C:\\Users", "C:\\ProgramData", "C:\\System Volume Information"
        };

        for (const auto& protectedPath : protectedPaths)
        {
            // Check if output is exactly or is a child of a protected path
            if (outputPath == protectedPath ||
                outputPath.startsWithIgnoreCase(protectedPath + "/") ||
                outputPath.startsWithIgnoreCase(protectedPath + "\\"))
            {
                // Allow /dev/shm (tmpfs RAM disk) for high-performance temp file operations
                if (outputPath.startsWithIgnoreCase("/dev/shm"))
                    continue;

                options.hasError = true;
                options.errorMessage = "output directory cannot be a system directory: " + outputPath;
                return options;
            }
        }
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
                               juce::String& errorMessage,
                               bool verbose = false)
{
    return PluginHostManager::discoverPlugin(
        hostManager.getFormatManager(), pluginFile, description, errorMessage, verbose);
}

bool loadPluginForInspection(PluginHostManager& hostManager,
                             const juce::PluginDescription& description,
                             const AudioFileInfo& audioInfo,
                             juce::String& errorMessage)
{
    hostManager.prepare(audioInfo.sampleRate, kDefaultBlockSize, audioInfo.numChannels);

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

    if (!discoverPluginDescription(hostManager, options.pluginFile, description, errorMessage, options.verbose))
    {
        std::cerr << "Warning: " << errorMessage << "\n";
    }

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
    discoverPluginDescription(hostManager, options.pluginFile, description, errorMessage, options.verbose);

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
        if (options.durationSeconds > 0.0)
            std::cout << "Duration limit: " << options.durationSeconds << " seconds\n";
    }

    OfflineBatchConfig config;
    config.inputFile = options.inputFile;
    config.outputDirectory = options.outputDirectory;
    config.pluginFile = options.pluginFile;
    config.configDirectory = options.configDirectory;
    config.totalVariations = options.variations;
    config.parallelWorkers = options.workers;
    config.enableSIMD = options.enableSimd;
    config.useMemoryPool = options.usePool;
    config.renderConfig.sampleRate = audioInfo.sampleRate;
    config.renderConfig.blockSize = 512;
    config.renderConfig.numChannels = audioInfo.numChannels;
    config.renderConfig.outputDirectory = options.outputDirectory;

    // Apply duration limit if specified
    if (options.durationSeconds > 0.0)
    {
        config.renderConfig.fullDuration = static_cast<float>(options.durationSeconds);
        config.renderConfig.transientDuration = static_cast<float>(options.durationSeconds);
        config.renderConfig.steadyStateDuration = static_cast<float>(options.durationSeconds);
        config.renderConfig.customDuration = static_cast<float>(options.durationSeconds);
    }

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
    config.configDirectory = options.configDirectory;
    config.totalVariations = options.variations;
    config.parallelWorkers = options.workers;
    config.enableSIMD = options.enableSimd;
    config.useMemoryPool = options.usePool;
    config.renderConfig.sampleRate = audioInfo.sampleRate;
    config.renderConfig.blockSize = 512;
    config.renderConfig.numChannels = audioInfo.numChannels;
    config.renderConfig.outputDirectory = options.outputDirectory;
    config.renderConfig.validateOutput = true;

    // When --config-dir is provided, derive totalVariations from the number of
    // JSON files so the renderer processes every config exactly once.
    if (options.configDirectory.isDirectory())
    {
        const auto jsonCount = options.configDirectory
            .getNumberOfChildFiles(juce::File::findFiles, "*.json");
        if (jsonCount > 0)
            config.totalVariations = jsonCount;
    }

    // Apply duration limit if specified
    if (options.durationSeconds > 0.0)
    {
        config.renderConfig.fullDuration = static_cast<float>(options.durationSeconds);
        config.renderConfig.transientDuration = static_cast<float>(options.durationSeconds);
        config.renderConfig.steadyStateDuration = static_cast<float>(options.durationSeconds);
        config.renderConfig.customDuration = static_cast<float>(options.durationSeconds);
    }

    OfflineBatchRenderer renderer;
    if (!renderer.setConfig(config))
    {
        std::cerr << "Error: failed to configure renderer\n";
        return 1;
    }

    // Wire up ParameterSafetyConfig to prevent the "Parameter Trap":
    // locks dangerous binary params (Mute, Solo, Bypass, Phase Invert) and
    // constrains safe DSP params (Frequency, Gain, Q, Shape, Slope) to sane ranges.
    {
        // Query parameter count from the renderer's already-loaded plugin
        // (avoids loading the plugin a second time in a separate host).
        int numParams = 0;
        if (const auto* plugin = renderer.getLoadedPlugin())
            numParams = plugin->getParameters().size();

        if (numParams == 0)
        {
            std::cerr << "Warning: Could not query plugin parameters, using default safety profile\n";
            numParams = kFallbackParamCount;
        }

        const juce::String pluginName = options.pluginFile.getFileNameWithoutExtension();
        auto safetyProfile = ParameterSafetyConfig::createAutoProfile(pluginName, numParams);
        renderer.setSafetyConfig(safetyProfile);
        renderer.setSafeRandomizationEnabled(true);

        if (options.verbose)
        {
            auto counts = safetyProfile.getCategoryCounts();
            std::cout << "Safety profile: " << pluginName << " (" << safetyProfile.getRuleCount() << " rules)\n";
            std::cout << "  Plugin has " << numParams << " parameters\n";
            for (const auto& [cat, count] : counts)
                std::cout << "  " << parameterCategoryToString(cat) << ": " << count << "\n";
        }
    }

    // NOTE: startRender() runs synchronously — renderLoop() executes on the
    // calling thread and returns only after all variations are complete.
    // The onRenderComplete callback fires BEFORE startRender() returns.
    bool renderSuccess = false;
    juce::String renderMessage = "Rendering did not complete";

    renderer.onProgressUpdate = [](const OfflineBatchProgress& progress)
    {
        std::cout << "\rProgress: " << progress.completed << "/" << progress.total
                  << " (" << std::fixed << std::setprecision(1) << progress.percentage << "%)   "
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
        renderSuccess = success;
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

    if (!renderer.startRender(nullptr))
    {
        std::cerr << "Error: failed to start renderer\n";
        return 1;
    }

    std::cout << "\n";

    if (options.verbose)
        printProfilerSummary(renderer.getProfiler());

    const auto progress = renderer.getProgress();
    const bool success = renderSuccess && progress.failedRenders == 0;

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

    if (options.version)
    {
        std::cout << "morephi-dataset " << kVersion << "\n";
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
