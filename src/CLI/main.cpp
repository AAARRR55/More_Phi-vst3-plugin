/*
 * MorphSnap — CLI/main.cpp
 * Command-line interface for MorphSnap dataset generation.
 * Provides batch processing with performance optimization options.
 */

#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Host/PluginHostManager.h"
#include <juce_core/juce_core.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <iomanip>

using namespace morphsnap;

/** Print usage information for batch processing */
static void printBatchUsage()
{
    std::cout <<
        "MorphSnap Offline Batch Processor v3.3.0\n"
        "Renders audio through VST3 plugins faster-than-real-time.\n\n"
        "Usage:\n"
        "  morphsnap-dataset --plugin <path.vst3> --input <dry.wav> [options]\n"
        "  morphsnap-dataset --config <config.json>\n\n"
        "Performance Options:\n"
        "  --workers N             Number of parallel worker threads (default: auto-detect)\n"
        "  --no-simd              Disable SIMD audio optimizations\n"
        "  --no-memory-pool       Disable memory pool for audio buffers\n"
        "  --profile              Enable performance profiling output\n\n"
        "Basic Options:\n"
        "  --plugin <path.vst3>   Path to VST3 plugin to process\n"
        "  --input <audio.wav>    Input audio file to process\n"
        "  --output <directory>   Output directory for rendered files\n"
        "  --variations N         Number of parameter variations (default: 100)\n"
        "  --help                 Show this help message\n\n"
        "Examples:\n"
        "  morphsnap-dataset --plugin MyPlugin.vst3 --input drum_loop.wav --output ./renders\n"
        "  morphsnap-dataset --plugin MyPlugin.vst3 --input audio.wav --workers 8 --profile\n"
        "  morphsnap-dataset --config batch_config.json --no-simd\n";
}

/** Parse command line arguments */
struct CliArgs
{
    std::string pluginPath;
    std::string inputFile;
    std::string outputDir = "./output";
    std::string configFile;
    int variations = 100;
    int workers = std::thread::hardware_concurrency();
    bool enableSIMD = true;
    bool useMemoryPool = true;
    bool enableProfiling = false;
    bool showHelp = false;
    bool hasError = false;
    std::string errorMessage;
};

static CliArgs parseArgs(int argc, char* argv[])
{
    CliArgs args;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg(argv[i]);

        if (arg == "--help" || arg == "-h")
        {
            args.showHelp = true;
        }
        else if (arg == "--plugin" && i + 1 < argc)
        {
            args.pluginPath = argv[++i];
        }
        else if (arg == "--input" && i + 1 < argc)
        {
            args.inputFile = argv[++i];
        }
        else if (arg == "--output" && i + 1 < argc)
        {
            args.outputDir = argv[++i];
        }
        else if (arg == "--config" && i + 1 < argc)
        {
            args.configFile = argv[++i];
        }
        else if (arg == "--variations" && i + 1 < argc)
        {
            args.variations = std::stoi(argv[++i]);
        }
        else if (arg == "--workers" && i + 1 < argc)
        {
            args.workers = std::stoi(argv[++i]);
        }
        else if (arg == "--no-simd")
        {
            args.enableSIMD = false;
        }
        else if (arg == "--no-memory-pool")
        {
            args.useMemoryPool = false;
        }
        else if (arg == "--profile")
        {
            args.enableProfiling = true;
        }
        else
        {
            args.hasError = true;
            args.errorMessage = "Unknown argument: " + arg;
            break;
        }
    }

    // Validate required arguments (if not showing help)
    if (!args.showHelp && args.configFile.empty())
    {
        if (args.pluginPath.empty())
        {
            args.hasError = true;
            args.errorMessage = "Missing required argument: --plugin";
        }
        else if (args.inputFile.empty())
        {
            args.hasError = true;
            args.errorMessage = "Missing required argument: --input";
        }
    }

    return args;
}

/** Main entry point */
int main(int argc, char* argv[])
{
    auto args = parseArgs(argc, argv);

    if (args.showHelp)
    {
        printBatchUsage();
        return 0;
    }

    if (args.hasError)
    {
        std::cerr << "Error: " << args.errorMessage << "\n\n";
        printBatchUsage();
        return 1;
    }

    // Initialize JUCE
    juce::initialiseJuce_GUI();

    std::cout << "MorphSnap Dataset Generator v3.3.0\n";
    std::cout << "Performance optimizations: ";
    std::cout << "Workers=" << args.workers << ", ";
    std::cout << "SIMD=" << (args.enableSIMD ? "ON" : "OFF") << ", ";
    std::cout << "MemoryPool=" << (args.useMemoryPool ? "ON" : "OFF") << "\n\n";

    // Create configuration
    OfflineBatchConfig config;
    config.inputFile = juce::File(args.inputFile);
    config.outputDirectory = juce::File(args.outputDir);
    config.totalVariations = args.variations;
    config.parallelWorkers = args.workers;
    config.enableSIMD = args.enableSIMD;
    config.useMemoryPool = args.useMemoryPool;

    // Validate configuration
    if (!config.isValid())
    {
        std::cerr << "Invalid configuration:\n";
        if (!config.inputFile.existsAsFile())
            std::cerr << "  - Input file does not exist: " << args.inputFile << "\n";
        if (!config.outputDirectory.isDirectory())
            std::cerr << "  - Output directory does not exist: " << args.outputDir << "\n";
        return 1;
    }

    // Initialize renderer
    OfflineBatchRenderer renderer;
    if (!renderer.setConfig(config))
    {
        std::cerr << "Failed to initialize renderer with configuration\n";
        return 1;
    }

    // Set up progress callback
    renderer.onProgressUpdate = [](const OfflineBatchProgress& progress)
    {
        std::cout << "Progress: " << progress.completed << "/" << progress.total
                  << " (" << std::fixed << std::setprecision(1) << progress.percentage << "%)"
                  << " [" << progress.successfulRenders << " success, "
                  << progress.failedRenders << " failed]\r" << std::flush;
    };

    renderer.onRenderComplete = [&args](bool success, const juce::String& message)
    {
        std::cout << "\n";
        if (success)
        {
            std::cout << "Batch rendering completed successfully!\n";
            if (args.enableProfiling)
            {
                std::cout << "Performance statistics available in profiler.\n";
            }
        }
        else
        {
            std::cerr << "Batch rendering failed: " << message << "\n";
        }
    };

    // Start rendering
    std::cout << "Starting batch rendering...\n";
    if (!renderer.startRender())
    {
        std::cerr << "Failed to start rendering process\n";
        return 1;
    }

    // Wait for completion
    while (renderer.isRendering())
    {
        juce::Thread::sleep(100);
    }

    // Print profiling results if enabled
    if (args.enableProfiling)
    {
        std::cout << "\nPerformance Statistics:\n";
        const auto& profiler = renderer.getProfiler();
        for (const auto& [name, stats] : profiler.getAllStats())
        {
            std::cout << "  " << name << ": "
                      << stats.averageMs << "ms avg, "
                      << stats.totalMs << "ms total, "
                      << stats.callCount << " calls\n";
        }
    }

    juce::shutdownJuce_GUI();
    return 0;
}