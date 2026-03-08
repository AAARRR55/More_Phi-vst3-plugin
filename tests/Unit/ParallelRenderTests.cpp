#include <catch2/catch_test_macros.hpp>
#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Host/PluginHostManager.h"

TEST_CASE("Parallel renderer processes variations concurrently", "[ParallelRender]")
{
    morphsnap::OfflineBatchConfig config;
    config.inputFile = juce::File("test_input.wav");
    config.outputDirectory = juce::File::getCurrentWorkingDirectory().getChildFile("test_output");
    config.totalVariations = 8;
    config.parallelWorkers = 4;

    morphsnap::OfflineBatchRenderer renderer;
    renderer.setConfig(config);

    REQUIRE(renderer.getParallelWorkerCount() == 4);
}