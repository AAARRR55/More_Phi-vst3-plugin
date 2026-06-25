#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "Core/PerformanceProfiler.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

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

// AUDIT-2026-06-25 (M4): the profiler now keeps a trailing ring buffer per
// section and reports p50/p95/p99 from it. These tests pin the new contract.
TEST_CASE("PerformanceProfiler reports trailing-window percentiles", "[PerformanceProfiler]")
{
    more_phi::PerformanceProfiler profiler;
    profiler.registerSection("pct_section");

    // Feed a known, sorted-able sample set. We record explicit durations via
    // recordTime (deterministic — avoids sleep jitter) so we can assert ranks.
    // Samples in ms: 1..20 inclusive.
    for (int i = 1; i <= 20; ++i)
        profiler.recordTime("pct_section", static_cast<double>(i));

    const auto stats = profiler.getStats("pct_section");
    REQUIRE(stats.callCount == 20);

    // All 20 samples fit in the ring (kRingSamples = 2048), so percentiles are
    // over the full population. Nearest-rank: rank = ceil(q/100 * n).
    //   p50: ceil(0.50*20) = 10  -> window[9]  = 10.0
    //   p95: ceil(0.95*20) = 19  -> window[18] = 19.0
    //   p99: ceil(0.99*20) = 20  -> window[19] = 20.0
    REQUIRE_THAT(stats.p50Ms, Catch::Matchers::WithinAbs(10.0, 1e-9));
    REQUIRE_THAT(stats.p95Ms, Catch::Matchers::WithinAbs(19.0, 1e-9));
    REQUIRE_THAT(stats.p99Ms, Catch::Matchers::WithinAbs(20.0, 1e-9));

    // Running mean/min/max still hold.
    REQUIRE_THAT(stats.minTimeMs, Catch::Matchers::WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(stats.maxTimeMs, Catch::Matchers::WithinAbs(20.0, 1e-9));
    REQUIRE_THAT(stats.averageTimeMs, Catch::Matchers::WithinAbs(10.5, 1e-9));
}

TEST_CASE("PerformanceProfiler ring caps at kRingSamples (trailing window)", "[PerformanceProfiler]")
{
    more_phi::PerformanceProfiler profiler;
    profiler.registerSection("overflow_section");

    // Push more samples than the ring holds. kRingSamples is power-of-two 2048.
    // The ring should keep the MOST RECENT 2048; percentiles then reflect only
    // those, not the full history. We assert the trailing-window semantics by
    // pushing values [0..3000) and checking p99 lands in the recent window.
    constexpr int kPush = 3000;
    for (int i = 0; i < kPush; ++i)
        profiler.recordTime("overflow_section", static_cast<double>(i));

    const auto stats = profiler.getStats("overflow_section");
    REQUIRE(static_cast<int>(stats.callCount) == kPush);

    // The trailing window holds samples [952..3000). p99 nearest-rank over 2048
    // samples: ceil(0.99*2048) = 2028 -> window[2027] = 952 + 2027 = 2979.
    // So p99 must be >= 952 (the oldest kept value) — if the ring were unbounded
    // it would be ~2970 too, but the key assertion is it is NOT the global p99
    // of the full 3000-sample set (which would be 2970). We assert the ring is
    // bounded: p50 of the trailing window is well above the global median.
    REQUIRE(stats.p99Ms >= 952.0);
    REQUIRE(stats.p50Ms > 1500.0); // trailing median >> global median (~1500)
}

TEST_CASE("PerformanceProfiler recordTime never blocks (audio-thread contract)", "[PerformanceProfiler]")
{
    // C-2/C-16 invariant: recordTime uses a try-lock. If a reader holds the
    // lock, the sample is DROPPED, never blocked. We verify by holding the
    // reader lock from another thread while the "audio" thread records — the
    // record must complete in bounded time (well under a typical block).
    more_phi::PerformanceProfiler profiler;
    profiler.registerSection("rt_section");
    profiler.prepare();

    std::atomic<bool> readerHolding{ true };
    std::thread reader([&]()
    {
        // Spin so the try-lock in recordTime consistently misses. We can't grab
        // the profiler's internal SpinLock directly (it's private), so we rely
        // on getStats() taking the blocking lock briefly; the real guarantee we
        // test is that recordTime returns promptly regardless.
        while (readerHolding.load(std::memory_order_relaxed))
        {
            (void) profiler.getStats("rt_section"); // blocks on the internal lock briefly
        }
    });

    // Give the reader a moment to start contending.
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // Time a burst of recordTime calls. Even under contention each must be
    // microsecond-scale (try-lock + maybe drop). We assert a generous upper
    // bound: 100k records in well under 2 seconds.
    const auto t0 = std::chrono::high_resolution_clock::now();
    constexpr int kRecords = 100000;
    for (int i = 0; i < kRecords; ++i)
        profiler.recordTime("rt_section", 0.001);
    const auto t1 = std::chrono::high_resolution_clock::now();
    const double totalMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    readerHolding.store(false, std::memory_order_relaxed);
    reader.join();

    // No allocation, no blocking: well under 2s for 100k records even with
    // contention. (A blocking lock under contention would blow past this.)
    REQUIRE(totalMs < 2000.0);
    // callCount may be < kRecords (drops are allowed) but must be > 0.
    const auto stats = profiler.getStats("rt_section");
    REQUIRE(stats.callCount > 0);
    REQUIRE(static_cast<int>(stats.callCount) <= kRecords);
}
