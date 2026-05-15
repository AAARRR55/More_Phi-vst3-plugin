#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "AI/MCPToolHandler.h"

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
    }

    REQUIRE(foundToolsCallAlias);
    REQUIRE(foundProfileAudit);
    REQUIRE(foundPlanPreview);
    REQUIRE(foundRenderStatus);
}
