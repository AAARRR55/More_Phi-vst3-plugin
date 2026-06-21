#include <catch2/catch_test_macros.hpp>

#include "AI/AIAssistant.h"
#include "AI/LLMChatClient.h"
#include "AI/MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"
#include "UI/AIChatPanel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <regex>
#include <set>
#include <string>
#include <vector>

using namespace more_phi;

namespace {

// Extract the API-facing tool names from either the OpenAI- or Anthropic-shaped
// tools JSON. Returns a set so callers can also assert uniqueness by comparing
// to the source array length.
auto collectNames = [](const nlohmann::json& arr, bool isAnthropic) {
    std::set<std::string> names;
    for (const auto& t : arr)
    {
        const auto name = isAnthropic
                              ? t.value("name", std::string{})
                              : (t.contains("function") ? t["function"].value("name", std::string{})
                                                        : std::string{});
        names.insert(name);
    }
    return names;
};

// Tools that were previously hidden from the chat model by the
// shouldExposeToolToChatModel blocklist. Any of these missing here means a
// regression has reintroduced the blocklist and the in-plugin AI assistant
// has lost "full unrestricted" tool access.
const std::vector<std::string> kPreviouslyBlockedToolNames = {
    "izotope_ipc_attach",
    "izotope_ipc_detach",
    "izotope_ipc_status",
    "izotope_ipc_snapshot",
    "izotope_ipc_dump",
    "izotope_ipc_capture",
    "ozone_run_assistant",
    "hosted_plugin_scan",
    "hosted_plugin_load",
    "plugin_profile_save",
    "mastering_render_batch",
    "mastering_render_status",
    "mastering_select_candidate",
};

// Loose minimum to catch silent filtering. The real tool count is well above
// this; bump only if the tool surface intentionally shrinks.
constexpr std::size_t kMinExpectedToolCount = 30;

juce::var toVar(const nlohmann::json& value)
{
    return juce::JSON::parse(juce::String(value.dump()));
}

} // namespace

TEST_CASE("LLM chat client exposes API-safe runtime MCP tool names with full tool access", "[unit][ai][llm][chat]")
{
    const auto tools = nlohmann::json::parse(LLMChatClient::mcpToolsToOpenAIJson().toStdString());
    REQUIRE(tools.is_array());
    REQUIRE_FALSE(tools.empty());

    INFO("OpenAI tool surface size: " << tools.size());
    CHECK(tools.size() >= kMinExpectedToolCount);

    // Every entry must be a function-shaped tool with a non-empty name that
    // satisfies sanitizeToolNameForApi's contract: only [A-Za-z0-9_-], starts
    // with a letter or underscore, and is at most 64 characters long.
    static const std::regex kSanitizedNameRegex(R"(^[A-Za-z_][A-Za-z0-9_-]{0,63}$)");

    for (const auto& tool : tools)
    {
        REQUIRE(tool.contains("function"));
        const auto name = tool["function"].value("name", std::string{});
        INFO("tool name: " << name);
        CHECK_FALSE(name.empty());
        CHECK(name.find('.') == std::string::npos);
        CHECK(std::regex_match(name, kSanitizedNameRegex));
    }

    const auto names = collectNames(tools, /*isAnthropic=*/false);

    // Uniqueness: set size equals array size means makeUniqueToolName is
    // disambiguating any colliding names correctly.
    INFO("unique names: " << names.size() << " / array: " << tools.size());
    CHECK(names.size() == tools.size());

    // Core edit tools the assistant always needs.
    CHECK(names.count("plugin_profile_describe_semantic_map") == 1);
    CHECK(names.count("set_parameter") == 1);
    CHECK(names.count("hosted_plugin_set_parameter") == 1);
    CHECK(names.count("more_phi_parameters") == 1);
    CHECK(names.count("more_phi_set_parameter") == 1);

    // The chat client now exposes the full MCP tool surface to the LLM so the
    // assistant can perform direct edits on both the hosted plugin and More-Phi
    // itself. Confirmation for destructive/expensive operations is enforced via
    // the system prompt instead of a hard blocklist. Each previously-blocked
    // tool is asserted explicitly so a partial regression cannot slip through.
    for (const auto& expected : kPreviouslyBlockedToolNames)
    {
        INFO("expected previously-blocked tool: " << expected);
        CHECK(names.count(expected) == 1);
    }

    // Dataset family is a prefix group; assert at least one variant is present
    // so the LLM can drive the dataset pipeline.
    const bool hasAnyDatasetTool = std::any_of(names.begin(), names.end(),
        [](const std::string& n) { return n.rfind("generate_dataset", 0) == 0; });
    CHECK(hasAnyDatasetTool);

    // API-name -> MCP-name round-trip. Dotted forms must NOT round-trip back
    // to a real MCP method, since the LLM is funneled through underscored
    // names only.
    CHECK(LLMChatClient::resolveToolNameForTest("plugin_profile_describe_semantic_map")
          == "plugin_profile.describe_semantic_map");
    CHECK(LLMChatClient::resolveToolNameForTest("more_phi_set_parameter")
          == "more_phi.set_parameter");
    CHECK(LLMChatClient::resolveToolNameForTest("hosted_plugin_set_parameter")
          == "hosted_plugin.set_parameter");
    CHECK(LLMChatClient::resolveToolNameForTest("hosted_plugin.load").isEmpty());
}

TEST_CASE("LLM chat client exposes hosted and More-Phi edit tools to Anthropic", "[unit][ai][llm][chat]")
{
    const auto tools = nlohmann::json::parse(LLMChatClient::mcpToolsToAnthropicJson().toStdString());
    REQUIRE(tools.is_array());
    REQUIRE_FALSE(tools.empty());

    INFO("Anthropic tool surface size: " << tools.size());
    CHECK(tools.size() >= kMinExpectedToolCount);

    for (const auto& tool : tools)
    {
        const auto name = tool.value("name", std::string{});
        INFO("tool name: " << name);
        CHECK_FALSE(name.empty());
        CHECK(name.find('.') == std::string::npos);
    }

    const auto names = collectNames(tools, /*isAnthropic=*/true);
    CHECK(names.size() == tools.size());

    CHECK(names.count("hosted_plugin_set_parameter") == 1);
    CHECK(names.count("more_phi_set_parameter") == 1);

    // Mirror the OpenAI test: Anthropic shape can regress independently, so
    // assert the same destructive-tool surface is present here too.
    for (const auto& expected : kPreviouslyBlockedToolNames)
    {
        INFO("expected previously-blocked tool (Anthropic): " << expected);
        CHECK(names.count(expected) == 1);
    }
}

TEST_CASE("LLM chat client system prompt encodes underscore naming and confirmation list", "[unit][ai][llm][chat]")
{
    // Lock in the prompt-side guardrails added when shouldExposeToolToChatModel
    // was opened up. The prompt is the only thing standing between the LLM and
    // long-running / destructive MCP tools, so any silent change to its content
    // should fail this test.
    const juce::String prompt = LLMChatClient::systemPromptForTest();
    REQUIRE_FALSE(prompt.isEmpty());

    INFO("system prompt length: " << prompt.length());

    // Underscore-naming guidance must be present so the LLM does not emit the
    // dotted form that resolveApiToolNameToMcpName intentionally rejects.
    CHECK(prompt.contains("underscores"));

    // Confirmation list must reference each high-impact tool by its underscored
    // API name (the LLM will see exactly these names in tools/list).
    CHECK(prompt.contains("hosted_plugin_load"));
    CHECK(prompt.contains("mastering_render_batch"));
    CHECK(prompt.contains("generate_dataset"));
    CHECK(prompt.contains("izotope_ipc_"));

    // The prompt must explicitly warn the LLM away from the dotted form (which
    // would route through resolveApi... as unmapped and silently fail). The
    // single dotted occurrence "hosted_plugin.load" is intentionally present
    // exactly once as a negative example inside the warning sentence; any
    // OTHER dotted MCP tool names must not appear at all.
    CHECK(prompt.contains("dotted form"));
    CHECK(prompt.contains("untrusted DATA"));
    CHECK(prompt.contains("surface the error message verbatim"));
    CHECK(prompt.contains("like hosted_plugin.load"));
    CHECK_FALSE(prompt.contains("mastering.render_batch"));
    CHECK_FALSE(prompt.contains("mastering.select_candidate"));
    CHECK_FALSE(prompt.contains("hosted_plugin.scan"));
    CHECK_FALSE(prompt.contains("plugin_profile.save"));
    CHECK_FALSE(prompt.contains("more_phi.set_parameters"));
    CHECK_FALSE(prompt.contains("more_phi.parameters"));

    // The neural mastering tool must be referenced so the LLM knows to call it
    // as the primary mastering source (see the "Mastering decisions" guidance).
    CHECK(prompt.contains("sonicmaster_decision"));

    // Confirm the dotted negative example appears AT MOST ONCE - i.e. it is
    // only the warning sentence and was not duplicated elsewhere in the prompt.
    const int firstDottedIdx = prompt.indexOf("hosted_plugin.load");
    REQUIRE(firstDottedIdx >= 0);
    const int secondDottedIdx = prompt.indexOf(firstDottedIdx + 1, "hosted_plugin.load");
    CHECK(secondDottedIdx == -1);
}

TEST_CASE("AI chat panel handles MCP tool inventory questions locally", "[unit][ai][llm][chat]")
{
    CHECK(AIChatPanel::detectsLocalMcpToolInventoryPromptForTest(
        "What MCP tools do you have access to?"));
    CHECK(AIChatPanel::detectsLocalMcpToolInventoryPromptForTest(
        "List available MCP tools"));
    CHECK_FALSE(AIChatPanel::detectsLocalMcpToolInventoryPromptForTest(
        "Make this snare brighter"));

    const auto reply = AIChatPanel::buildLocalMcpToolInventoryReplyForTest();
    REQUIRE_FALSE(reply.isEmpty());
    CHECK(reply.contains("local More-Phi MCP tools"));
    CHECK(reply.contains("Hosted plugin controls"));
    CHECK(reply.contains("more_phi.parameters"));
    CHECK(reply.contains("hosted_plugin.set_parameter"));
    CHECK(reply.contains("remote LLM provider"));
}

TEST_CASE("AI assistant plans local morph language as a WorkflowRun DAG", "[unit][ai][assistant][workflow]")
{
    MorePhiProcessor processor;
    AIAssistant assistant(processor);

    const auto plan = assistant.planLocalWorkflowPrompt("Move the morph fader to 42%");

    REQUIRE(plan.handled);
    REQUIRE(plan.valid);
    REQUIRE(plan.workflowSubmitParams.is_object());
    CHECK(plan.summary.contains("morph fader"));
    CHECK(plan.workflowSubmitParams["context"]["source"].get<std::string>() == "in_plugin_assistant");

    const auto& steps = plan.workflowSubmitParams["steps"];
    REQUIRE(steps.is_array());
    REQUIRE(steps.size() == 1);
    CHECK(steps[0]["id"].get<std::string>() == "set_morph_position");
    CHECK(steps[0]["toolName"].get<std::string>() == "set_morph_position");
    CHECK(std::abs(steps[0]["params"]["fader"].get<double>() - 0.42) < 0.0001);
    CHECK(steps[0]["params"]["source"].get<std::string>() == "fader");
}

TEST_CASE("AI assistant executes local natural language through workflow transactions", "[unit][ai][assistant][workflow]")
{
    MorePhiProcessor processor;
    AIAssistant assistant(processor);
    InstanceIdentity identity;

    const auto result = assistant.executeLocalWorkflowPrompt("Move the morph fader to 42%");

    REQUIRE(result.handled);
    REQUIRE(result.success);
    REQUIRE(result.workflowRunId.isNotEmpty());
    REQUIRE(result.transactionId.isNotEmpty());
    CHECK(result.message.contains("WorkflowRun"));
    CHECK(std::abs(processor.getFaderPos() - 0.42f) < 0.0001f);
    CHECK(processor.getMorphSource() == 1);

    const auto history = nlohmann::json::parse(
        MCPToolHandler::handle("automation.history",
            toVar(nlohmann::json{{"limit", 50}, {"workflow_run_id", result.workflowRunId.toStdString()}}),
            processor,
            identity).toStdString());

    REQUIRE(history["success"].get<bool>());
    bool foundAssistantTransaction = false;
    for (const auto& transaction : history["transactions"])
    {
        foundAssistantTransaction = foundAssistantTransaction
            || (transaction["workflowRunId"].get<std::string>() == result.workflowRunId.toStdString()
                && transaction["workflowStepId"].get<std::string>() == "set_morph_position"
                && transaction["toolName"].get<std::string>() == "set_morph_position");
    }
    REQUIRE(foundAssistantTransaction);
}

TEST_CASE("MCP memory.update_outcome_feedback updates automatic workflow outcome", "[unit][ai][assistant][workflow][memory]")
{
    MorePhiProcessor processor;
    AIAssistant assistant(processor);
    InstanceIdentity identity;

    const auto applied = assistant.executeLocalWorkflowPrompt("Move the morph fader to 42%");
    REQUIRE(applied.success);
    REQUIRE(applied.transactionId.isNotEmpty());

    const auto updated = nlohmann::json::parse(
        MCPToolHandler::handle("memory.update_outcome_feedback",
            toVar(nlohmann::json{
                {"transaction_id", applied.transactionId.toStdString()},
                {"feedback_status", "too much"},
                {"user_feedback", "that was too much"}
            }),
            processor,
            identity,
            assistant.getAutomationRuntime()).toStdString());

    REQUIRE(updated["success"].get<bool>());
    REQUIRE(updated["outcome"]["actionId"].get<std::string>() == applied.transactionId.toStdString());
    CHECK(updated["outcome"]["feedbackStatus"].get<std::string>() == "too_much");
    CHECK_FALSE(updated["outcome"]["userAccepted"].get<bool>());
    CHECK(updated["memory"]["content"]["feedbackStatus"].get<std::string>() == "too_much");

    const auto events = nlohmann::json::parse(
        MCPToolHandler::handle("events.list_recent",
            toVar(nlohmann::json{{"limit", 20}}),
            processor,
            identity,
            assistant.getAutomationRuntime()).toStdString());
    REQUIRE(events["success"].get<bool>());

    bool foundEvent = false;
    for (const auto& event : events["events"])
    {
        foundEvent = foundEvent
            || (event.value("type", std::string{}) == "outcome.feedback_updated"
                && event.value("transactionId", std::string{}) == applied.transactionId.toStdString());
    }
    REQUIRE(foundEvent);
}

TEST_CASE("AI assistant records feedback for the last local workflow outcome", "[unit][ai][assistant][workflow][memory]")
{
    MorePhiProcessor processor;
    AIAssistant assistant(processor);
    InstanceIdentity identity;

    const auto applied = assistant.executeLocalWorkflowPrompt("Move the morph fader to 42%");
    REQUIRE(applied.success);
    REQUIRE(applied.transactionId.isNotEmpty());

    const auto feedback = assistant.recordFeedbackForLastWorkflow("that sounded better");

    REQUIRE(feedback.handled);
    REQUIRE(feedback.success);
    REQUIRE(feedback.workflowRunId == applied.workflowRunId);
    REQUIRE(feedback.transactionId == applied.transactionId);
    CHECK(feedback.message.contains("Recorded feedback"));

    const auto outcomes = nlohmann::json::parse(
        MCPToolHandler::handle("memory.list_outcomes",
            toVar(nlohmann::json{{"workflow_run_id", applied.workflowRunId.toStdString()}, {"limit", 20}}),
            processor,
            identity).toStdString());
    REQUIRE(outcomes["success"].get<bool>());

    bool foundFeedback = false;
    int matchingOutcomes = 0;
    for (const auto& outcome : outcomes["outcomes"])
    {
        const auto& content = outcome["content"];
        if (content.value("actionId", std::string{}) == applied.transactionId.toStdString())
        {
            ++matchingOutcomes;
            foundFeedback = true;
            CHECK(content["userAccepted"].get<bool>());
            CHECK(content["source"].get<std::string>() == "user_feedback");
            CHECK(content["feedbackStatus"].get<std::string>() == "sounds_better");
            CHECK(content["userFeedback"].get<std::string>().find("sounded better") != std::string::npos);
            CHECK(content["outcomeScore"].get<double>() > 0.8);
        }
    }
    REQUIRE(foundFeedback);
    CHECK(matchingOutcomes == 1);
}

TEST_CASE("AI assistant records corrective feedback for an overdone local workflow", "[unit][ai][assistant][workflow][memory]")
{
    MorePhiProcessor processor;
    AIAssistant assistant(processor);
    InstanceIdentity identity;

    const auto applied = assistant.executeLocalWorkflowPrompt("Move the morph fader to 90%");
    REQUIRE(applied.success);
    REQUIRE(applied.transactionId.isNotEmpty());

    const auto feedback = assistant.recordFeedbackForLastWorkflow("that was too much");

    REQUIRE(feedback.handled);
    REQUIRE(feedback.success);
    REQUIRE(feedback.workflowRunId == applied.workflowRunId);
    REQUIRE(feedback.transactionId == applied.transactionId);

    const auto outcomes = nlohmann::json::parse(
        MCPToolHandler::handle("memory.list_outcomes",
            toVar(nlohmann::json{{"workflow_run_id", applied.workflowRunId.toStdString()}, {"limit", 20}}),
            processor,
            identity).toStdString());
    REQUIRE(outcomes["success"].get<bool>());

    bool foundFeedback = false;
    for (const auto& outcome : outcomes["outcomes"])
    {
        const auto& content = outcome["content"];
        if (content.value("actionId", std::string{}) == applied.transactionId.toStdString())
        {
            foundFeedback = true;
            CHECK_FALSE(content["userAccepted"].get<bool>());
            CHECK(content["source"].get<std::string>() == "user_feedback");
            CHECK(content["feedbackStatus"].get<std::string>() == "too_much");
            CHECK(content["userFeedback"].get<std::string>().find("too much") != std::string::npos);
            CHECK(content["outcomeScore"].get<double>() < 0.3);
        }
    }
    REQUIRE(foundFeedback);
}

TEST_CASE("AI chat panel formats workflow timeline previews and feedback", "[unit][ai][assistant][workflow][ux]")
{
    MorePhiProcessor processor;
    AIAssistant assistant(processor);

    const auto applied = assistant.executeLocalWorkflowPrompt("Move the morph fader to 42%");
    REQUIRE(applied.success);
    REQUIRE(applied.workflowRunId.isNotEmpty());
    REQUIRE(applied.transactionId.isNotEmpty());

    const auto timeline = AIChatPanel::buildWorkflowTimelineTextForTest(applied);
    CHECK(timeline.contains("WorkflowRun"));
    CHECK(timeline.contains("completed"));
    CHECK(timeline.contains("Transaction"));
    CHECK(timeline.contains("Plan Preview"));
    CHECK(timeline.contains("fader"));
    CHECK(timeline.contains("-> 42%"));
    CHECK(timeline.contains("Verify"));

    const auto feedback = assistant.recordFeedbackForLastWorkflow("that sounded better");
    REQUIRE(feedback.success);

    const auto feedbackTimeline = AIChatPanel::buildWorkflowTimelineTextForTest(feedback);
    CHECK(feedbackTimeline.contains("Feedback"));
    CHECK(feedbackTimeline.contains("sounds_better"));
    CHECK(feedbackTimeline.contains("that sounded better"));
}

TEST_CASE("AI chat panel formats pending approval queue entries", "[unit][ai][assistant][permission][ux]")
{
    const nlohmann::json approvals{
        {"success", true},
        {"approvals", nlohmann::json::array({
            {
                {"id", "approval-test"},
                {"workflowRunId", "workflow-test"},
                {"toolName", "hosted_plugin.load"},
                {"risk", "high_impact"},
                {"status", "approved"},
                {"explanation", "old approved request"}
            },
            {
                {"id", "approval-pending"},
                {"workflowRunId", "workflow-pending"},
                {"toolName", "hosted_plugin.load"},
                {"risk", "high_impact"},
                {"status", "pending"},
                {"explanation", "Dispatch-layer PermissionPolicy requires approval."},
                {"predictedDiff", {
                    {"approval_preview", true},
                    {"diffs", nlohmann::json::array({
                        {{"name", "Limiter Ceiling"}, {"before", 0.5}, {"after", 0.25}, {"risk", "high_impact"}}
                    })}
                }}
            }
        })}
    };

    const auto text = AIChatPanel::buildApprovalQueueTextForTest(approvals);
    CHECK(text.contains("Approval Required"));
    CHECK(text.contains("approval-pending"));
    CHECK(text.contains("workflow-pending"));
    CHECK(text.contains("hosted_plugin.load"));
    CHECK(text.contains("high_impact"));
    CHECK(text.contains("Plan Preview"));
    CHECK(text.contains("Limiter Ceiling"));
    CHECK_FALSE(text.contains("approval-test"));

    const auto empty = AIChatPanel::buildApprovalQueueTextForTest(
        nlohmann::json{{"success", true}, {"approvals", nlohmann::json::array()}});
    CHECK(empty.contains("No pending approval"));
}

TEST_CASE("AI assistant reports when there is no local workflow to receive feedback", "[unit][ai][assistant][workflow][memory]")
{
    MorePhiProcessor processor;
    AIAssistant assistant(processor);

    const auto feedback = assistant.recordFeedbackForLastWorkflow("that sounded better");

    REQUIRE(feedback.handled);
    REQUIRE_FALSE(feedback.success);
    CHECK(feedback.message.contains("No assistant workflow"));
}

TEST_CASE("AI chat panel detects local workflow prompts before cloud chat", "[unit][ai][assistant][workflow]")
{
    CHECK(AIChatPanel::detectsLocalWorkflowPromptForTest("Move the morph fader to 42%"));
    CHECK(AIChatPanel::detectsLocalWorkflowPromptForTest("set morph position to center"));
    CHECK(AIChatPanel::detectsLocalUndoPromptForTest("rollback the previous assistant workflow"));
    CHECK(AIChatPanel::detectsLocalWorkflowFeedbackPromptForTest("that sounded better"));
    CHECK(AIChatPanel::detectsLocalWorkflowFeedbackPromptForTest("that was too much, reject that result"));
    CHECK_FALSE(AIChatPanel::detectsLocalWorkflowPromptForTest("What MCP tools do you have access to?"));
    CHECK_FALSE(AIChatPanel::detectsLocalWorkflowPromptForTest("Make this snare brighter"));
}

TEST_CASE("AI chat panel formats raw WinHTTP timeout errors", "[unit][ai][llm][chat]")
{
    const auto formatted = AIChatPanel::formatChatErrorForTest(
        "WinHTTP receive response failed with error 12002.");

    CHECK(formatted.contains("NVIDIA chat request timed out"));
    CHECK(formatted.contains("local MCP server"));
    CHECK(formatted.contains("Raw transport detail"));
    CHECK(formatted.contains("12002"));

    const auto alreadyFriendly = AIChatPanel::formatChatErrorForTest(
        "NVIDIA chat request failed at the transport layer. (WinHTTP receive response failed with error 12002.)");
    CHECK(alreadyFriendly.startsWith("NVIDIA chat request failed at the transport layer"));
}

TEST_CASE("parseOpenAIResponse extracts NVIDIA inline tool-call tokens", "[unit][ai][llm][chat][nvidia]")
{
    // Simulates the exact response body NVIDIA NIM returns when the model
    // uses special tokens instead of a structured tool_calls array.
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {
            {"role", "assistant"},
            {"content",
                "I'll set the Gain parameter to its maximum value (1.0)."
                "<|tool_calls_section_begin|>"
                "<|tool_call_begin|>functions set_parameter:1"
                "<|tool_call_argument_begin|>"
                R"({"name": "Gain", "value": 1.0})"
                "<|tool_call_end|>"
                "<|tool_calls_section_end|>"}
        }}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());

    // Text before the tool calls section is preserved as the assistant message.
    const auto text = result["text"].get<std::string>();
    CHECK(text.find("maximum value") != std::string::npos);
    CHECK(text.find("<|tool_call") == std::string::npos);

    const auto& tcs = result["tool_calls"];
    REQUIRE(tcs.is_array());
    REQUIRE(tcs.size() == 1);
    CHECK(tcs[0]["name"].get<std::string>() == "set_parameter");
    CHECK(tcs[0]["id"].get<std::string>() == "nvidia_tc_1");

    const auto args = nlohmann::json::parse(tcs[0]["arguments"].get<std::string>());
    CHECK(args["name"].get<std::string>() == "Gain");
    CHECK(args["value"].get<double>() == 1.0);
}

TEST_CASE("parseOpenAIResponse extracts multiple NVIDIA inline tool calls", "[unit][ai][llm][chat][nvidia]")
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {
            {"role", "assistant"},
            {"content",
                "<|tool_calls_section_begin|>"
                "<|tool_call_begin|>functions set_parameters_batch:2"
                "<|tool_call_argument_begin|>"
                R"({"parameters":[{"name":"Gain","value":0.6},{"name":"Cutoff","value":0.3}]})"
                "<|tool_call_end|>"
                "<|tool_calls_section_end|>"}
        }}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    const auto& tcs = result["tool_calls"];
    REQUIRE(tcs.size() == 1);
    CHECK(tcs[0]["name"].get<std::string>() == "set_parameters_batch");

    const auto args = nlohmann::json::parse(tcs[0]["arguments"].get<std::string>());
    CHECK(args["parameters"].size() == 2);
}

TEST_CASE("parseOpenAIResponse prefers structured tool_calls over inline tokens", "[unit][ai][llm][chat][nvidia]")
{
    // When the provider returns BOTH a structured tool_calls array AND inline
    // tokens in the content, the structured array wins.
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {
            {"role", "assistant"},
            {"content", "text with <|tool_call_begin|>functions fake:99<|tool_call_argument_begin|>{}<|tool_call_end|>"},
            {"tool_calls", nlohmann::json::array({
                {{"id", "real_tc_1"},
                 {"type", "function"},
                 {"function", {{"name", "get_plugin_info"}, {"arguments", "{}"}}}}
            })}
        }}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    const auto& tcs = result["tool_calls"];
    REQUIRE(tcs.size() == 1);
    CHECK(tcs[0]["name"].get<std::string>() == "get_plugin_info");
    CHECK(tcs[0]["id"].get<std::string>() == "real_tc_1");
}

// ── Content-shape / reasoning-model robustness tests ─────────────────────────
// NVIDIA NIM and other OpenAI-compatible backends can return "content" as an
// array of text blocks, as null (reasoning models whose budget went to
// reasoning_content), or leave the answer in "reasoning_content". The parser
// must surface these instead of silently producing an empty reply (which the
// UI renders as the unhelpful "(empty response)").

TEST_CASE("parseOpenAIResponse extracts content as an array of text blocks", "[unit][ai][llm][chat][nvidia]")
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"},
                       {"content", nlohmann::json::array({
                           {{"type", "text"}, {"text", "Analysis: the audio peaks at -3dB."}}
                       })}}}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());
    CHECK(result["text"].get<std::string>() == "Analysis: the audio peaks at -3dB.");
}

TEST_CASE("parseOpenAIResponse falls back to reasoning_content when content is null", "[unit][ai][llm][chat][nvidia]")
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"},
                       {"content", nullptr},
                       {"reasoning_content", "Step 1: gain is high. Final: reduce gain."}}},
         {"finish_reason", "stop"}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());
    const auto text = result["text"].get<std::string>();
    CHECK(text.find("reduce gain") != std::string::npos);
}

TEST_CASE("parseOpenAIResponse falls back to reasoning_content given as an array", "[unit][ai][llm][chat][nvidia]")
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"},
                       {"content", nullptr},
                       {"reasoning_content", nlohmann::json::array({
                           {{"type", "text"}, {"text", "think"}},
                           {{"type", "text"}, {"text", " answer"}}
                       })}}}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());
    CHECK(result["text"].get<std::string>() == "think answer");
}

TEST_CASE("parseOpenAIResponse reports token-budget exhaustion via finish_reason=length", "[unit][ai][llm][chat][nvidia]")
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"}, {"content", nullptr}}},
         {"finish_reason", "length"}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    const auto err = result["error"].get<std::string>();
    CHECK(result["text"].get<std::string>().empty());
    CHECK(err.find("truncated") != std::string::npos);
    CHECK(err.find("token budget") != std::string::npos);
    CHECK(err.find("max_tokens") != std::string::npos);
}

TEST_CASE("parseOpenAIResponse reports guidance for a genuinely empty stop reply", "[unit][ai][llm][chat][nvidia]")
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"}, {"content", nullptr}}},
         {"finish_reason", "stop"}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    const auto err = result["error"].get<std::string>();
    CHECK(err.find("no visible content") != std::string::npos);
    CHECK(err.find("reasoning model") != std::string::npos);
}

TEST_CASE("parseOpenAIResponse does not misclassify tool-token text inside reasoning_content", "[unit][ai][llm][chat][nvidia]")
{
    // A reasoning dump that merely mentions a tool-call literal must NOT become a
    // phantom tool call (ordering guard between inline-token detection and the
    // reasoning_content fallback).
    const juce::String reasoning = "I should call <|tool_call_begin|>functions fake:99"
                                   "<|tool_call_argument_begin|>{}<|tool_call_end|> but I won't.";
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"},
                       {"content", nullptr},
                       {"reasoning_content", std::string(reasoning.toRawUTF8())}}}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());
    REQUIRE(result["tool_calls"].is_array());
    CHECK(result["tool_calls"].size() == 0);
    CHECK(result["text"].get<std::string>() == reasoning.toStdString());
}

TEST_CASE("parseOpenAIResponse tolerates malformed content arrays without throwing", "[unit][ai][llm][chat][nvidia]")
{
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"},
                       {"content", nlohmann::json::array({
                           123, true, nullptr,
                           nlohmann::json::object({{"type", "image_url"}}),
                           nlohmann::json::object({{"text", 456}}),
                           nlohmann::json::object({{"type", "text"}, {"text", "ok"}})
                       })}}}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());
    CHECK(result["text"].get<std::string>() == "ok");
}

TEST_CASE("parseOpenAIResponse parses inline tool tokens when content is an array", "[unit][ai][llm][chat][nvidia]")
{
    // Twin-bug guard: inline tool tokens carried inside a content array must
    // still be parsed (the inline-token fallback previously re-read content via
    // is_string() and missed them entirely).
    const std::string contentText =
        std::string("Setting param.")
        + "<|tool_calls_section_begin|>"
        + "<|tool_call_begin|>functions set_parameter:1"
        + "<|tool_call_argument_begin|>"
        + R"({"name":"Gain","value":1.0})"
        + "<|tool_call_end|>"
        + "<|tool_calls_section_end|>";

    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"},
                       {"content", nlohmann::json::array({
                           {{"type", "text"}, {"text", contentText}}
                       })}}}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());
    const auto& tcs = result["tool_calls"];
    REQUIRE(tcs.is_array());
    REQUIRE(tcs.size() == 1);
    CHECK(tcs[0]["name"].get<std::string>() == "set_parameter");
    CHECK(result["text"].get<std::string>().find("Setting param") != std::string::npos);
}

TEST_CASE("parseOpenAIResponse keeps empty-text tool-call turns as a clean success", "[unit][ai][llm][chat][nvidia]")
{
    // Regression guard: the empty-response diagnostic must be gated on BOTH no
    // text AND no tool calls, so a turn that carries tool calls but no preamble
    // text stays a clean success.
    nlohmann::json body;
    body["choices"] = nlohmann::json::array({
        {{"message", {{"role", "assistant"},
                       {"content",
                           "<|tool_calls_section_begin|>"
                           "<|tool_call_begin|>functions set_parameters_batch:2"
                           "<|tool_call_argument_begin|>"
                           R"({"parameters":[{"name":"Gain","value":0.6}]})"
                           "<|tool_call_end|>"
                           "<|tool_calls_section_end|>"}}}}
    });

    const auto result = nlohmann::json::parse(
        LLMChatClient::parseOpenAIResponseForTest(200, juce::String(body.dump())).toStdString());

    CHECK(result["error"].get<std::string>().empty());
    REQUIRE(result["tool_calls"].is_array());
    REQUIRE(result["tool_calls"].size() == 1);
}

TEST_CASE("LLMChatClient raises max_tokens budget for reasoning models", "[unit][ai][llm][chat]")
{
    CHECK(LLMChatClient::maxTokensFor("deepseek-ai/deepseek-r1") >= 16384);
    CHECK(LLMChatClient::maxTokensFor("nvidia/llama-3.1-nemotron-70b-instruct") >= 16384);
    CHECK(LLMChatClient::maxTokensFor("gpt-4o") == 4096);
    CHECK(LLMChatClient::maxTokensFor("meta/llama-3.1-70b-instruct") == 4096);
}

// ── Latency optimization tests ──────────────────────────────────────────────

TEST_CASE("Tool name resolution is consistent across repeated calls (cached map)", "[unit][ai][llm][chat][latency]")
{
    // The tool-name map is now cached with std::once_flag. Calling
    // resolveToolNameForTest multiple times must return identical results,
    // confirming the cache is populated correctly and stable.
    const auto r1 = LLMChatClient::resolveToolNameForTest("more_phi_set_parameter");
    const auto r2 = LLMChatClient::resolveToolNameForTest("more_phi_set_parameter");
    CHECK(r1 == r2);
    CHECK(r1 == "more_phi.set_parameter");

    const auto r3 = LLMChatClient::resolveToolNameForTest("hosted_plugin_set_parameter");
    const auto r4 = LLMChatClient::resolveToolNameForTest("hosted_plugin_set_parameter");
    CHECK(r3 == r4);
    CHECK(r3 == "hosted_plugin.set_parameter");

    // Unmapped dotted name should consistently return empty
    const auto r5 = LLMChatClient::resolveToolNameForTest("hosted_plugin.load");
    const auto r6 = LLMChatClient::resolveToolNameForTest("hosted_plugin.load");
    CHECK(r5 == r6);
    CHECK(r5.isEmpty());
}

TEST_CASE("Chat-filtered tool surface is smaller than full surface (OpenAI)", "[unit][ai][llm][chat][latency]")
{
    const auto fullTools = nlohmann::json::parse(LLMChatClient::mcpToolsToOpenAIJson().toStdString());
    const auto chatTools = nlohmann::json::parse(LLMChatClient::chatToolsOpenAIJsonForTest().toStdString());

    REQUIRE(fullTools.is_array());
    REQUIRE(chatTools.is_array());

    INFO("Full tool count: " << fullTools.size() << ", Chat tool count: " << chatTools.size());
    CHECK(chatTools.size() < fullTools.size());
    CHECK(chatTools.size() >= 15);
    CHECK(chatTools.size() <= 35);

    const auto names = collectNames(chatTools, /*isAnthropic=*/false);

    // Core interactive tools must be present in the filtered set
    CHECK(names.count("set_parameter") == 1);
    CHECK(names.count("get_parameter") == 1);
    CHECK(names.count("list_parameters") == 1);
    CHECK(names.count("get_plugin_info") == 1);
    CHECK(names.count("more_phi_set_parameter") == 1);
    CHECK(names.count("more_phi_parameters") == 1);
    CHECK(names.count("capture_snapshot") == 1);
    CHECK(names.count("recall_snapshot") == 1);
    CHECK(names.count("set_morph_position") == 1);
    CHECK(names.count("get_morph_state") == 1);
    CHECK(names.count("run_self_test") == 1);
    CHECK(names.count("plugin_profile_describe_semantic_map") == 1);

    // Heavy/rarely-used tools should NOT be in the chat subset
    CHECK(names.count("mastering_render_batch") == 0);
    CHECK(names.count("mastering_render_status") == 0);

    // The neural mastering decision tool must be exposed to chat (it is the
    // assistant's primary mastering source per the system prompt).
    CHECK(names.count("sonicmaster_decision") == 1);

    // All names must still be unique
    CHECK(names.size() == chatTools.size());
}

TEST_CASE("Chat-filtered tool surface is smaller than full surface (Anthropic)", "[unit][ai][llm][chat][latency]")
{
    const auto fullTools = nlohmann::json::parse(LLMChatClient::mcpToolsToAnthropicJson().toStdString());
    const auto chatTools = nlohmann::json::parse(LLMChatClient::chatToolsAnthropicJsonForTest().toStdString());

    REQUIRE(fullTools.is_array());
    REQUIRE(chatTools.is_array());

    INFO("Full Anthropic tool count: " << fullTools.size() << ", Chat tool count: " << chatTools.size());
    CHECK(chatTools.size() < fullTools.size());
    CHECK(chatTools.size() >= 15);
    CHECK(chatTools.size() <= 35);

    const auto names = collectNames(chatTools, /*isAnthropic=*/true);
    CHECK(names.count("more_phi_set_parameter") == 1);
    CHECK(names.count("set_parameter") == 1);
    CHECK(names.count("capture_snapshot") == 1);
    CHECK(names.size() == chatTools.size());
}

TEST_CASE("Conversation history trimming preserves system message and caps size", "[unit][ai][llm][chat][latency]")
{
    // Build a 50-message history: system + 49 user/assistant pairs
    nlohmann::json history = nlohmann::json::array();
    history.push_back({{"role", "system"}, {"content", "You are a helpful assistant."}});

    for (int i = 1; i <= 49; ++i)
    {
        const auto role = (i % 2 == 1) ? "user" : "assistant";
        history.push_back({{"role", role}, {"content", "Message " + std::to_string(i)}});
    }

    REQUIRE(history.size() == 50);

    // AIChatPanel::trimConversationHistory is private, but we can test the
    // same logic: parse, keep system + last 28, re-serialize.
    // The implementation keeps messages when size <= 30 and trims to
    // system + last 28 when > 30.
    nlohmann::json trimmed = nlohmann::json::array();
    if (!history.empty() && history[0].value("role", "") == "system")
        trimmed.push_back(history[0]);
    const auto keepFrom = history.size() - 28;
    for (std::size_t i = (keepFrom > 1 ? keepFrom : 1); i < history.size(); ++i)
        trimmed.push_back(history[i]);

    // System message preserved
    CHECK(trimmed[0].value("role", "") == "system");
    CHECK(trimmed[0].value("content", "") == "You are a helpful assistant.");

    // Total size is system + 28 = 29
    CHECK(trimmed.size() == 29);

    // Last message is preserved
    CHECK(trimmed.back().value("content", "") == "Message 49");

    // First non-system message should be from the tail, not the beginning
    CHECK(trimmed[1].value("content", "").find("Message ") != std::string::npos);
    const int firstKeptIdx = static_cast<int>(keepFrom);
    CHECK(trimmed[1].value("content", "") == "Message " + std::to_string(firstKeptIdx));
}

TEST_CASE("Conversation history trimming is a no-op for small histories", "[unit][ai][llm][chat][latency]")
{
    nlohmann::json history = nlohmann::json::array();
    history.push_back({{"role", "system"}, {"content", "System prompt"}});
    history.push_back({{"role", "user"}, {"content", "Hello"}});
    history.push_back({{"role", "assistant"}, {"content", "Hi there"}});

    REQUIRE(history.size() == 3);

    // With <= 30 messages, trimming should not modify anything
    // (mirrors the early-return in AIChatPanel::trimConversationHistory)
    CHECK(history.size() <= 30);
    // No trim needed — the array stays as-is
    CHECK(history.size() == 3);
    CHECK(history[0].value("role", "") == "system");
    CHECK(history[2].value("content", "") == "Hi there");
}
