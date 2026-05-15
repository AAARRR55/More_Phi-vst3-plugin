#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "AI/MCPToolHandler.h"
#include "AI/TrackAssistantStore.h"

namespace {

struct ScopedTrackAssistantStore
{
    ScopedTrackAssistantStore()
    {
        directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile("morephi_track_assistant_store_unit", "");
        directory.createDirectory();
        more_phi::TrackAssistantStore::setStoreDirectoryOverrideForTests(directory);
    }

    ~ScopedTrackAssistantStore()
    {
        more_phi::TrackAssistantStore::clearStoreDirectoryOverrideForTests();
        directory.deleteRecursively();
    }

    juce::File directory;
};

} // namespace

TEST_CASE("MCP server audit placeholder compiles", "[mcp]")
{
    REQUIRE(true);
}

TEST_CASE("MCP tools/list exposes standard and mastering workflow tools", "[mcp][tools]")
{
    const auto listed = nlohmann::json::parse(more_phi::MCPToolHandler::getToolList().toStdString());

    REQUIRE(listed.contains("tools"));
    REQUIRE(listed["tools"].is_array());

    bool foundToolsCallAlias = false;
    bool foundProfileAudit = false;
    bool foundPlanPreview = false;
    bool foundRenderStatus = false;
    bool foundOzoneGetInfo = false;
    bool foundOzoneUpdateStatus = false;
    bool foundOzoneAnalyze = false;
    bool foundOzoneSearch = false;
    bool foundOzoneGetInfoUnderscore = false;
    bool foundOzoneAnalyzeUnderscore = false;

    for (const auto& tool : listed["tools"])
    {
        REQUIRE(tool.contains("name"));
        REQUIRE(tool.contains("inputSchema"));
        REQUIRE(tool["inputSchema"].is_object());

        const auto name = tool["name"].get<std::string>();
        if (name == "hosted_plugin.parameters")
            foundToolsCallAlias = true;
        if (name == "plugin_profile.audit_parameters")
            foundProfileAudit = true;
        if (name == "mastering.plan_preview")
            foundPlanPreview = true;
        if (name == "mastering.render_status")
            foundRenderStatus = true;
        if (name == "ozone.track.get_info")
            foundOzoneGetInfo = true;
        if (name == "ozone.track.update_status")
            foundOzoneUpdateStatus = true;
        if (name == "ozone.track.analyze")
            foundOzoneAnalyze = true;
        if (name == "ozone.track.search")
            foundOzoneSearch = true;
        if (name == "ozone_track_get_info")
            foundOzoneGetInfoUnderscore = true;
        if (name == "ozone_track_analyze")
            foundOzoneAnalyzeUnderscore = true;
    }

    REQUIRE(foundToolsCallAlias);
    REQUIRE(foundProfileAudit);
    REQUIRE(foundPlanPreview);
    REQUIRE(foundRenderStatus);
    REQUIRE(foundOzoneGetInfo);
    REQUIRE(foundOzoneUpdateStatus);
    REQUIRE(foundOzoneAnalyze);
    REQUIRE(foundOzoneSearch);
    REQUIRE(foundOzoneGetInfoUnderscore);
    REQUIRE(foundOzoneAnalyzeUnderscore);
}

TEST_CASE("TrackAssistantStore persists local track records", "[mcp][track-assistant]")
{
    ScopedTrackAssistantStore scopedStore;

    const auto sourceFile = scopedStore.directory.getChildFile("Midnight Drive.wav");
    REQUIRE(sourceFile.replaceWithText("placeholder"));

    const auto track = more_phi::TrackAssistantStore::upsertFileTrack(sourceFile, "render_unit_1");
    REQUIRE(track.contains("track_id"));

    const auto trackId = juce::String(track["track_id"].get<std::string>());
    REQUIRE(more_phi::TrackAssistantStore::isValidTrackId(trackId));

    const auto search = more_phi::TrackAssistantStore::search(
        "Midnight", {}, juce::String(), juce::String(), 1, 20);
    REQUIRE(search["success"].get<bool>());
    REQUIRE(search["total"].get<int>() == 1);

    const auto updated = more_phi::TrackAssistantStore::updateStatus(trackId, "on_hold", "needs review");
    REQUIRE(updated["success"].get<bool>());
    REQUIRE(updated["status"].get<std::string>() == "on_hold");

    const auto info = more_phi::TrackAssistantStore::getInfo(trackId, true);
    REQUIRE(info["success"].get<bool>());
    REQUIRE(info["history"].is_array());
    REQUIRE(info["history"].size() >= 2);
}
