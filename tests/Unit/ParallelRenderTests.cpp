#include <catch2/catch_test_macros.hpp>
#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Host/PluginHostManager.h"

TEST_CASE("Parallel renderer processes variations concurrently", "[ParallelRender]")
{
    // Create a minimal temp file so setConfig's existsAsFile() check passes.
    // The file only needs to exist — we are not running a render here.
    auto inputFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_parallel_input", ".wav");
    inputFile.create();

    const auto outputDirectory = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_parallel_output", "");

    more_phi::OfflineBatchConfig config;
    config.inputFile = inputFile;
    config.outputDirectory = outputDirectory;
    config.totalVariations = 8;
    config.parallelWorkers = 4;

    more_phi::OfflineBatchRenderer renderer;
    REQUIRE(renderer.setConfig(config));
    REQUIRE(renderer.getParallelWorkerCount() == 4);

    outputDirectory.deleteRecursively();
    inputFile.deleteFile();
}
