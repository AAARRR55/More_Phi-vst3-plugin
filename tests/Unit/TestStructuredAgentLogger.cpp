#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include "AI/Agents/Logging/StructuredAgentLogger.h"

using namespace more_phi::agents;

TEST_CASE("StructuredAgentLogger writes one JSON object per line", "[agents][logging]")
{
    const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("more-phi-agent-log-test-"
                                            + juce::String(juce::Time::getHighResolutionTicks()));
    tempDir.deleteRecursively();
    tempDir.createDirectory();

    StructuredAgentLogger logger(tempDir, "run-abc");

    logger.log("analysis-1", "info", "took a measurement",
               { { "taskId", "task-1" }, { "lufs", -14.2 } });
    logger.log("realtime-1", "warn", "clipping corrected",
               { { "param", 42 } });

    REQUIRE(logger.flushedCount() == 2);
    REQUIRE(logger.bufferedCount() == 0);
    REQUIRE(logger.activeFile().existsAsFile());

    const auto text = logger.activeFile().loadFileAsString().toStdString();
    // Two newline-terminated JSON lines.
    REQUIRE(std::count(text.begin(), text.end(), '\n') == 2);

    tempDir.deleteRecursively();
}

TEST_CASE("StructuredAgentLogger parses back as valid JSON with required fields", "[agents][logging]")
{
    const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("more-phi-agent-log-test2-"
                                            + juce::String(juce::Time::getHighResolutionTicks()));
    tempDir.deleteRecursively();
    tempDir.createDirectory();

    StructuredAgentLogger logger(tempDir, "run-xyz");
    logger.log("conductor-1", "debug", "decomposed goal", { { "intent", "louder" } });

    juce::StringArray lines;
    logger.activeFile().readLines(lines);
    REQUIRE(lines.size() >= 1);

    const auto parsed = nlohmann::json::parse(lines[0].toStdString());
    REQUIRE(parsed["agent"]   == "conductor-1");
    REQUIRE(parsed["level"]   == "debug");
    REQUIRE(parsed["message"] == "decomposed goal");
    REQUIRE(parsed["run_id"]  == "run-xyz");
    REQUIRE(parsed["fields"]["intent"] == "louder");
    REQUIRE(parsed.contains("ts_ms"));

    tempDir.deleteRecursively();
}

TEST_CASE("StructuredAgentLogger buffers in-memory when no directory is supplied", "[agents][logging]")
{
    StructuredAgentLogger logger(juce::File{}, "run-buffered");
    logger.log("memory-1", "info", "compaction ran", { { "bytes", 4096 } });
    logger.log("memory-1", "info", "compaction ran again", {});

    REQUIRE(logger.flushedCount() == 0);
    REQUIRE(logger.bufferedCount() == 2);
    REQUIRE(! logger.activeFile().existsAsFile());
}

TEST_CASE("StructuredAgentLogger caps the in-memory ring at capacity", "[agents][logging]")
{
    StructuredAgentLogger logger(juce::File{}, "run-cap");
    // 300 records → ring should cap at 256 and drop the oldest 44.
    for (int i = 0; i < 300; ++i)
        logger.log("memory-1", "info", "msg", { { "i", i } });

    REQUIRE(logger.bufferedCount() == 256);
}
