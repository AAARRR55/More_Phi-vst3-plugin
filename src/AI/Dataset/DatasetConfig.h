/*
 * More-Phi — AI/Dataset/DatasetConfig.h
 * Configuration schema and CLI interface for dataset generation.
 */
#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <map>
#include <iostream>
#include <iomanip>

namespace more_phi {

/**
 * JSON schema for dataset configuration validation.
 */
namespace DatasetConfigSchema {

inline nlohmann::json getSchema()
{
    return {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "MorePhi Dataset Generator Configuration"},
        {"type", "object"},
        {"required", {"outputDirectory", "totalSamples"}},
        {"properties", {
            {"outputDirectory", {
                {"type", "string"},
                {"description", "Directory where the dataset will be created"}
            }},
            {"datasetName", {
                {"type", "string"},
                {"default", "morephi_dataset"},
                {"description", "Name of the dataset (used for directory name)"}
            }},
            {"totalSamples", {
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", 1000000},
                {"description", "Total number of samples to generate"}
            }},
            {"randomSeed", {
                {"type", "integer"},
                {"default", 42},
                {"description", "Random seed for reproducibility"}
            }},
            {"sampleRate", {
                {"type", "number"},
                {"enum", {44100, 48000, 96000}},
                {"default", 48000},
                {"description", "Sample rate for audio processing"}
            }},
            {"blockSize", {
                {"type", "integer"},
                {"enum", {256, 512, 1024, 2048}},
                {"default", 512},
                {"description", "Audio block size"}
            }},
            {"numChannels", {
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", 8},
                {"default", 2},
                {"description", "Number of audio channels"}
            }},
            {"outputFormat", {
                {"type", "string"},
                {"enum", {"WAV32Float", "WAV24", "FLAC24"}},
                {"default", "WAV32Float"},
                {"description", "Audio output format"}
            }},
            {"chainType", {
                {"type", "string"},
                {"enum", {"EQOnly", "DynamicsOnly", "Mastering", "Mixing", "Creative", "Custom"}},
                {"default", "Mastering"},
                {"description", "Plugin chain type"}
            }},
            {"chainConfigFile", {
                {"type", "string"},
                {"description", "Path to custom chain configuration JSON file"}
            }},
            {"sourceAudioDirectory", {
                {"type", "string"},
                {"description", "Directory containing source audio files"}
            }},
            {"useAugmentation", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Enable audio augmentation"}
            }},
            {"numParallelThreads", {
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", 32},
                {"default", 4},
                {"description", "Number of parallel rendering threads"}
            }},
            {"pluginSettleTimeMs", {
                {"type", "integer"},
                {"minimum", 0},
                {"maximum", 1000},
                {"default", 50},
                {"description", "Plugin settle time in milliseconds"}
            }},
            {"dryRun", {
                {"type", "boolean"},
                {"default", false},
                {"description", "Validate configuration without rendering"}
            }},
            {"enableValidation", {
                {"type", "boolean"},
                {"default", true},
                {"description", "Enable validation metrics"}
            }},
            {"samplingConfig", {
                {"type", "object"},
                {"properties", {
                    {"sampleCount", {{"type", "integer"}, {"default", 1000}}},
                    {"seed", {{"type", "integer"}, {"default", 42}}},
                    {"genreStrata", {
                        {"type", "array"},
                        {"items", {{"type", "string"}}},
                        {"description", "Genre stratification ratios (e.g., 'Electronic:0.25')"}
                    }}
                }}
            }},
            {"splitConfig", {
                {"type", "object"},
                {"properties", {
                    {"trainRatio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.7}}},
                    {"valRatio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.15}}},
                    {"testRatio", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"default", 0.15}}},
                    {"stratifyByGenre", {{"type", "boolean"}, {"default", true}}},
                    {"stratifyByIntensity", {{"type", "boolean"}, {"default", true}}},
                    {"randomSeed", {{"type", "integer"}, {"default", 42}}}
                }}
            }},
            {"featureExtraction", {
                {"type", "object"},
                {"properties", {
                    {"frameSize", {{"type", "integer"}, {"default", 2048}}},
                    {"hopSize", {{"type", "integer"}, {"default", 512}}},
                    {"computeMFCC", {{"type", "boolean"}, {"default", true}}},
                    {"computeChroma", {{"type", "boolean"}, {"default", true}}},
                    {"mfccCoefficients", {{"type", "integer"}, {"default", 13}}}
                }}
            }}
        }}
    };
}

} // namespace DatasetConfigSchema

/**
 * Command-line interface for dataset generation.
 */
class DatasetCLI
{
public:
    struct CLIOptions
    {
        juce::File configFile;
        juce::File outputDirectory;
        juce::File sourceDirectory;
        int totalSamples = 0;
        unsigned int seed = 0;
        bool dryRun = false;
        bool help = false;
        bool verbose = false;
        juce::String chainType;
        int numThreads = 0;
    };

    static CLIOptions parseArgs(int argc, char* argv[])
    {
        CLIOptions options;

        for (int i = 1; i < argc; ++i)
        {
            juce::String arg(argv[i]);

            if (arg == "-h" || arg == "--help")
            {
                options.help = true;
            }
            else if (arg == "-c" || arg == "--config")
            {
                if (i + 1 < argc)
                    options.configFile = juce::File(argv[++i]);
            else std::cerr << "Error: --config requires a file path\n";
            }
            else if (arg == "-o" || arg == "--output")
            {
                if (i + 1 < argc)
                    options.outputDirectory = juce::File(argv[++i]);
            else std::cerr << "Error: --output requires a directory path\n";
            }
            else if (arg == "-s" || arg == "--source")
            {
                if (i + 1 < argc)
                    options.sourceDirectory = juce::File(argv[++i]);
            else std::cerr << "Error: --source requires a directory path\n";
            }
            else if (arg == "-n" || arg == "--samples")
            {
                if (i + 1 < argc)
                    options.totalSamples = juce::String(argv[++i]).getIntValue();
            else std::cerr << "Error: --samples requires a number\n";
            }
            else if (arg == "--seed")
            {
                if (i + 1 < argc)
                    options.seed = juce::String(argv[++i]).getIntValue();
            else std::cerr << "Error: --seed requires a number\n";
            }
            else if (arg == "--dry-run")
            {
                options.dryRun = true;
            }
            else if (arg == "-v" || arg == "--verbose")
            {
                options.verbose = true;
            }
            else if (arg == "--chain")
            {
                if (i + 1 < argc)
                    options.chainType = argv[++i];
            else std::cerr << "Error: --chain requires a type\n";
            }
            else if (arg == "-t" || arg == "--threads")
            {
                if (i + 1 < argc)
                    options.numThreads = juce::String(argv[++i]).getIntValue();
            else std::cerr << "Error: --threads requires a number\n";
            }
        }

        return options;
    }

    static void printUsage()
    {
        std::cout << "MorePhi Dataset Generator v1.0\n\n";
        std::cout << "Usage: morephi-dataset [options]\n\n";
        std::cout << "Options:\n";
        std::cout << "  -h, --help           Show this help message\n";
        std::cout << "  -c, --config FILE    Configuration file (JSON/YAML)\n";
        std::cout << "  -o, --output DIR     Output directory\n";
        std::cout << "  -s, --source DIR     Source audio directory\n";
        std::cout << "  -n, --samples N      Total samples to generate\n";
        std::cout << "  --seed N             Random seed for reproducibility\n";
        std::cout << "  --dry-run            Validate without rendering\n";
        std::cout << "  -v, --verbose        Verbose output\n";
        std::cout << "  --chain TYPE         Chain type (EQOnly, DynamicsOnly, Mastering, Mixing, Creative)\n";
        std::cout << "  -t, --threads N      Number of parallel threads\n";
        std::cout << "\nExamples:\n";
        std::cout << "  morephi-dataset -c config.json\n";
        std::cout << "  morephi-dataset -o ./dataset -n 1000 --chain Mastering\n";
        std::cout << "  morephi-dataset -c config.json --dry-run\n";
    }

    static void printProgress(const GenerationProgress& progress)
    {
        std::cout << "\r[" << std::fixed << std::setprecision(1) << progress.percentage << "%] ";
        std::cout << "Sample " << progress.samplesCompleted << "/" << progress.totalSamples << " ";
        std::cout << "(" << progress.samplesPerHour << " samples/hour)";
        std::cout << std::flush;
    }

    static void printResult(const GenerationResult& result)
    {
        std::cout << "\n\n";

        if (result.success)
        {
            std::cout << "Generation completed successfully!\n\n";
        }
        else
        {
            std::cout << "Generation failed.\n\n";
        }

        std::cout << "Statistics:\n";
        std::cout << "  Samples generated: " << result.samplesGenerated << "\n";
        std::cout << "  Train samples: " << result.trainSamples << "\n";
        std::cout << "  Validation samples: " << result.valSamples << "\n";
        std::cout << "  Test samples: " << result.testSamples << "\n";
        std::cout << "  Total time: " << (result.totalTimeMs / 1000.0) << "s\n";
        std::cout << "  Throughput: " << result.samplesPerHour << " samples/hour\n";

        if (!result.errors.isEmpty())
        {
            std::cout << "\nErrors:\n";
            for (const auto& err : result.errors)
                std::cout << "  - " << err << "\n";
        }

        if (!result.warnings.isEmpty())
        {
            std::cout << "\nWarnings:\n";
            for (const auto& warn : result.warnings)
                std::cout << "  - " << warn << "\n";
        }

        std::cout << "\nOutput:\n";
        std::cout << "  Dataset: " << result.datasetDirectory.getFullPathName() << "\n";
        std::cout << "  Manifest: " << result.manifestFile.getFullPathName() << "\n";
    }
};

/**
 * Creates default configuration files for various use cases.
 */
class DatasetConfigFactory
{
public:
    static nlohmann::json createDefaultConfig()
    {
        return {
            {"outputDirectory", "./dataset"},
            {"datasetName", "morephi_dataset"},
            {"totalSamples", 1000},
            {"randomSeed", 42},
            {"sampleRate", 48000},
            {"blockSize", 512},
            {"numChannels", 2},
            {"outputFormat", "WAV32Float"},
            {"chainType", "Mastering"},
            {"useAugmentation", true},
            {"numParallelThreads", 4},
            {"pluginSettleTimeMs", 50},
            {"dryRun", false},
            {"enableValidation", true},
            {"samplingConfig", {
                {"sampleCount", 1000},
                {"seed", 42},
                {"genreStrata", {
                    "Electronic:0.25",
                    "RockPop:0.20",
                    "JazzAcoustic:0.15",
                    "HipHopRnB:0.15",
                    "Classical:0.05",
                    "Stems:0.15",
                    "TestSignals:0.05"
                }}
            }},
            {"splitConfig", {
                {"trainRatio", 0.7},
                {"valRatio", 0.15},
                {"testRatio", 0.15},
                {"stratifyByGenre", true},
                {"stratifyByIntensity", true},
                {"randomSeed", 42}
            }},
            {"featureExtraction", {
                {"frameSize", 2048},
                {"hopSize", 512},
                {"computeMFCC", true},
                {"computeChroma", true},
                {"mfccCoefficients", 13}
            }}
        };
    }

    static nlohmann::json createMasteringConfig()
    {
        auto config = createDefaultConfig();
        config["chainType"] = "Mastering";
        config["totalSamples"] = 5000;
        return config;
    }

    static nlohmann::json createEQConfig()
    {
        auto config = createDefaultConfig();
        config["chainType"] = "EQOnly";
        config["totalSamples"] = 2000;
        return config;
    }

    static nlohmann::json createDynamicsConfig()
    {
        auto config = createDefaultConfig();
        config["chainType"] = "DynamicsOnly";
        config["totalSamples"] = 3000;
        return config;
    }

    static nlohmann::json createQuickTestConfig()
    {
        auto config = createDefaultConfig();
        config["totalSamples"] = 10;
        config["dryRun"] = false;
        config["numParallelThreads"] = 1;
        return config;
    }

    static bool writeConfigToFile(const nlohmann::json& config, const juce::File& file)
    {
        std::ofstream ofs(file.getFullPathName().toStdString());
        ofs << config.dump(4);
        return ofs.good();
    }
};

} // namespace more_phi
