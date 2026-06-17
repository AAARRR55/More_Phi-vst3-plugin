#include <catch2/catch_test_macros.hpp>
#include "Core/PerformanceProfiler.h"
#include <thread>

TEST_CASE("PerformanceProfiler measures execution time", "[PerformanceProfiler]")
{
    more_phi::PerformanceProfiler profiler;
    profiler.registerSection("test_operation");

    {
        auto timer = profiler.createTimer("test_operation");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto stats = profiler.getStats("test_operation");
    REQUIRE(stats.callCount == 1);
    REQUIRE(stats.totalTimeMs >= 10.0);
    REQUIRE(stats.averageTimeMs >= 10.0);
}