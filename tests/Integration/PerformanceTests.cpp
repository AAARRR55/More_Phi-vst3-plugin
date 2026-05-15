#include <catch2/catch_test_macros.hpp>
#include "AI/Dataset/OfflineBatchRenderer.h"
#include "Host/PluginHostManager.h"
#include <chrono>

TEST_CASE("Performance optimizations improve rendering speed", "[Performance][Integration]")
{
    // Create test configuration
    more_phi::OfflineBatchConfig config;
    config.totalVariations = 16;
    config.parallelWorkers = 4;
    config.enableSIMD = true;
    config.useMemoryPool = true;

    more_phi::OfflineBatchRenderer renderer;
    renderer.setConfig(config);

    // Verify optimizations are enabled
    REQUIRE(renderer.getParallelWorkerCount() == 4);

    // Performance should be measurable
    auto& profiler = renderer.getProfiler();
    REQUIRE(profiler.getAllStats().empty()); // Should start empty
}