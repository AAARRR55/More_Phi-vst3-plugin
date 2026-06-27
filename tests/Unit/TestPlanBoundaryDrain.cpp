/*
 * AUDIT-F2.2 (2026-06-27): the P3.10 plan-boundary mechanism is an
 * observability signal, NOT cross-block all-or-nothing buffering (the
 * apply path enqueues each parameter write independently; a partially-
 * drained plan leaves the hosted plugin in a hybrid state across blocks).
 * The boundary command (paramIndex=-1, isPlanBoundary=true) lets a caller
 * detect via getLastDrainedPlanId() that a FULL plan committed to audio.
 *
 * Before this test there was NO coverage of that contract — zero tests
 * referenced getLastDrainedPlanId / enqueuePlanBoundary. This locks the
 * documented behaviour so a future regression (e.g. the stamp being
 * dropped, or the boundary command reaching the parameter-write path) is
 * caught:
 *   1. getLastDrainedPlanId() starts at 0 before any plan drains.
 *   2. After enqueuePlanBoundary(id) + drain, getLastDrainedPlanId() == id.
 *   3. A boundary that is still queued (not yet drained) does NOT advance
 *      the drained id — confirming the marker is an AFTER-THE-FACT commit
 *      signal, not a pre-drain guarantee (the intentional P3.10 risk).
 */
#include <catch2/catch_test_macros.hpp>

#include "Plugin/PluginProcessor.h"

#include <memory>

namespace
{
constexpr int kTestSampleRate = 48000;
constexpr int kBlockSize = 512;
} // namespace

TEST_CASE("getLastDrainedPlanId starts at 0 before any plan drains (F2.2)",
          "[audit][processor][F2-2]")
{
    more_phi::MorePhiProcessor processor;
    processor.prepareToPlay(static_cast<double>(kTestSampleRate), kBlockSize);

    REQUIRE(processor.getLastDrainedPlanId() == 0u);
}

TEST_CASE("enqueuePlanBoundary + drain advances getLastDrainedPlanId (F2.2)",
          "[audit][processor][F2-2]")
{
    more_phi::MorePhiProcessor processor;
    processor.prepareToPlay(static_cast<double>(kTestSampleRate), kBlockSize);

    REQUIRE(processor.enqueuePlanBoundary(42u));
    // Before the drain runs, the boundary is still queued -> drained id must NOT
    // have advanced. This is the core P3.10 contract: the marker is an
    // AFTER-THE-FACT commit signal, not a pre-drain guarantee.
    REQUIRE(processor.getLastDrainedPlanId() == 0u);

    // Drain the queue (no hosted plugin wired; boundary carries paramIndex=-1
    // so it writes nothing). A large maxCommands ensures the boundary is reached.
    constexpr int kParamCount = 4096;
    processor.drainParameterCommandQueue(kParamCount, /*maxCommands=*/256,
                                         /*exclusivePlugin=*/nullptr);

    // After the drain consumes the boundary command, the drained id == 42.
    REQUIRE(processor.getLastDrainedPlanId() == 42u);
}

TEST_CASE("a still-queued boundary does not advance the drained id (F2.2 partial-drain)",
          "[audit][processor][F2-2]")
{
    more_phi::MorePhiProcessor processor;
    processor.prepareToPlay(static_cast<double>(kTestSampleRate), kBlockSize);

    // Enqueue enough parameter writes to fill the drain budget BEFORE the
    // boundary, then the boundary. A small maxCommands means the drain stops
    // before reaching the boundary.
    std::vector<more_phi::MorePhiProcessor::ParamCommand> writes;
    writes.reserve(8);
    for (int i = 0; i < 8; ++i)
    {
        more_phi::MorePhiProcessor::ParamCommand c {};
        c.paramIndex = i;
        c.value = 0.5f;
        c.source = more_phi::MorePhiProcessor::ParameterEditSource::MCP;
        writes.push_back(c);
    }
    REQUIRE(processor.enqueueParameterBatch(writes));
    REQUIRE(processor.enqueuePlanBoundary(99u));

    // Drain only 4 commands — the boundary (queued after 8 writes) is NOT reached.
    constexpr int kParamCount = 4096;
    processor.drainParameterCommandQueue(kParamCount, /*maxCommands=*/4,
                                         /*exclusivePlugin=*/nullptr);

    // The boundary has not drained, so the drained id must still be 0. A caller
    // comparing its planId (99) to getLastDrainedPlanId() (0) detects the plan
    // is still partial — exactly the observability contract P3.10 provides.
    REQUIRE(processor.getLastDrainedPlanId() == 0u);
}
