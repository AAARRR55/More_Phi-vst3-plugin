#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "AI/MCPToolHandler.h"
#include "AI/InstanceIdentity.h"
#include "AI/MCPServer.h"
#include "AI/MasteringCandidateScoring.h"
#include "AI/SonicMasterAnalysisEngine.h"
#include "AI/SonicMasterDecisionDecoder.h"
#include "AI/TrackAssistantStore.h"
#include "Plugin/PluginProcessor.h"

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

more_phi::ParameterBridge::ParameterDescriptor makeDescriptor(const char* name,
                                                              const char* display = "",
                                                              const char* label = "",
                                                              bool discrete = false,
                                                              bool boolean = false)
{
    more_phi::ParameterBridge::ParameterDescriptor descriptor;
    descriptor.name = name;
    descriptor.displayValue = display;
    descriptor.label = label;
    descriptor.discrete = discrete;
    descriptor.boolean = boolean;
    descriptor.numSteps = boolean ? 2 : (discrete ? 8 : 0);
    descriptor.defaultValue = 0.5f;
    descriptor.value = 0.5f;
    return descriptor;
}

void assignDescriptorIndices(std::vector<more_phi::ParameterBridge::ParameterDescriptor>& descriptors)
{
    for (int i = 0; i < static_cast<int>(descriptors.size()); ++i)
    {
        descriptors[static_cast<size_t>(i)].index = i;
        descriptors[static_cast<size_t>(i)].stableId = "test-" + juce::String(i);
    }
}

bool jsonArrayContainsString(const nlohmann::json& values, const std::string& expected)
{
    for (const auto& value : values)
        if (value.is_string() && value.get<std::string>() == expected)
            return true;
    return false;
}

const nlohmann::json* findSemanticControl(const nlohmann::json& values, const std::string& semanticId)
{
    for (const auto& value : values)
    {
        if (value.contains("semantic_id") && value["semantic_id"].get<std::string>() == semanticId)
            return &value;
    }
    return nullptr;
}

class StubSonicMasterSource final : public more_phi::ISonicMasterInferenceSource
{
public:
    [[nodiscard]] bool isAvailable() const noexcept override { return true; }

    bool infer(const float*, float* outDecision, std::size_t outCapacity) noexcept override
    {
        if (outDecision == nullptr || outCapacity < more_phi::kSonicMasterDecisionWidth)
            return false;

        decision_.fill(0.0f);
        decision_[more_phi::kSonicMasterEqGainOffset] = more_phi::kAdaptiveEqMaxGainDb;
        decision_[more_phi::kSonicMasterTargetLufsIdx] = -8.0f;
        decision_[more_phi::kSonicMasterTruePeakIdx] = -0.5f;

        for (std::size_t b = 0; b < more_phi::kSonicMasterCompBandCount; ++b)
        {
            const auto o = more_phi::kSonicMasterCompOffset + b * more_phi::kSonicMasterCompBandWidth;
            decision_[o + 0] = -20.0f;
            decision_[o + 1] = 2.5f;
            decision_[o + 2] = 15.0f;
            decision_[o + 3] = 150.0f;
            decision_[o + 4] = 0.0f;
            decision_[o + 5] = 2.0f;
        }

        decision_[more_phi::kSonicMasterCompOffset + 0] = -6.0f;
        decision_[more_phi::kSonicMasterCompOffset + 1] = 4.0f;
        decision_[more_phi::kSonicMasterCompOffset + 2] = 1.0f;
        decision_[more_phi::kSonicMasterCompOffset + 3] = 2.0f;
        decision_[more_phi::kSonicMasterCompOffset + 4] = 3.0f;
        decision_[more_phi::kSonicMasterCompOffset + 5] = 4.0f;
        decision_[more_phi::kSonicMasterStereoOffset + 0] = 0.5f;
        decision_[more_phi::kSonicMasterStereoOffset + 1] = -0.25f;

        std::copy_n(decision_.data(), more_phi::kSonicMasterDecisionWidth, outDecision);
        return true;
    }

private:
    std::array<float, more_phi::kSonicMasterDecisionWidth> decision_ {};
};

void feedSonicMasterWindow(more_phi::SonicMasterAnalysisEngine& engine, double sampleRate)
{
    const auto frames = static_cast<std::size_t>(
        std::llround(more_phi::kSonicMasterSegmentFrames * sampleRate / 44100.0)) + 1024;
    std::vector<float> left(frames, 0.0001f);
    std::vector<float> right(frames, 0.0001f);
    constexpr std::size_t kBlock = 512;

    engine.setActive(true);
    for (std::size_t offset = 0; offset < frames; offset += kBlock)
    {
        const auto count = std::min(kBlock, frames - offset);
        engine.capture(left.data() + offset, right.data() + offset, count);
    }
}

juce::var toVar(const nlohmann::json& value)
{
    return juce::JSON::parse(juce::String(value.dump()));
}

} // namespace

TEST_CASE("MCP server start/stop lifecycle", "[mcp][lifecycle]")
{
    // Verify the embedded MCP server can start on a non-zero local port and stop
    // cleanly without crashing. This is a runtime smoke test, not a full
    // JSON-RPC functional test (those are covered by TestMCPIntegration.cpp).
    more_phi::MorePhiProcessor processor;

    // port 0 is rejected by startServer; use a fixed local port for the smoke test.
    constexpr int kTestPort = 30001;
    auto identity = more_phi::InstanceIdentity::generate(kTestPort);

    more_phi::MCPServer server(processor);
    server.setIdentity(identity);

    REQUIRE_FALSE(server.isRunning());

    server.startServer(kTestPort);

    // Give the server thread a moment to start/binding; isRunning() reflects thread state.
    for (int i = 0; i < 50 && !server.isRunning(); ++i)
        juce::Thread::sleep(10);

    REQUIRE(server.isRunning());
    REQUIRE(server.getPort() == kTestPort);

    // Binding may fail in headless/CI environments (socket init, port in use),
    // so we only assert that the lifecycle is observed and can be torn down.
    INFO("server healthy = " << server.isHealthy()
         << ", error count = " << server.getErrorCount());

    server.stopServer();
    REQUIRE_FALSE(server.isRunning());
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
    bool foundOzoneAudit = false;
    bool foundSpectrum = false;
    bool foundStereoField = false;
    bool foundOzoneGetInfoUnderscore = false;
    bool foundOzoneAnalyzeUnderscore = false;
    bool foundIpcStatus = false;
    bool foundIpcSnapshot = false;
    bool foundIpcRunAssistant = false;
    bool foundSemanticCanonical = false;
    bool foundSemanticAlias = false;
    bool foundMorePhiParameters = false;
    bool foundMorePhiSetParameter = false;
    bool foundHostedPluginSetParameter = false;
    bool foundAutomationHistory = false;
    bool foundWorkflowCreate = false;
    bool foundWorkflowSubmit = false;
    bool foundWorkflowPredict = false;
    bool foundPermissionState = false;
    bool foundContextSession = false;
    bool foundMemoryRemember = false;
    bool foundMemoryUpdateOutcome = false;
    bool foundEventsRecent = false;
    bool foundPluginAdapterCapabilities = false;

    for (const auto& tool : listed["tools"])
    {
        REQUIRE(tool.contains("name"));
        REQUIRE(tool.contains("inputSchema"));
        REQUIRE(tool["inputSchema"].is_object());

        const auto name = tool["name"].get<std::string>();
        if (name == "hosted_plugin.parameters")
            foundToolsCallAlias = true;
        if (name == "hosted_plugin.set_parameter")
            foundHostedPluginSetParameter = true;
        if (name == "plugin_profile.audit_parameters")
            foundProfileAudit = true;
        if (name == "plugin_profile.describe_semantic_map" || name == "describe_plugin_semantic_map")
        {
            if (name == "plugin_profile.describe_semantic_map")
                foundSemanticCanonical = true;
            if (name == "describe_plugin_semantic_map")
                foundSemanticAlias = true;

            REQUIRE(tool["inputSchema"].contains("properties"));
            REQUIRE(tool["inputSchema"]["properties"].contains("plugin_id"));
            REQUIRE(tool["inputSchema"]["properties"].contains("include_raw_parameters"));
            REQUIRE(tool["inputSchema"]["properties"].contains("max_controls"));
        }
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
        if (name == "ozone.audit_parameters")
            foundOzoneAudit = true;
        if (name == "analysis.get_spectrum")
        {
            foundSpectrum = true;
            REQUIRE(tool["inputSchema"].contains("properties"));
            REQUIRE(tool["inputSchema"]["properties"].contains("resolution"));
        }
        if (name == "analysis.get_stereo_field")
            foundStereoField = true;
        if (name == "izotope_ipc_status")
            foundIpcStatus = true;
        if (name == "izotope_ipc_snapshot")
            foundIpcSnapshot = true;
        if (name == "ozone_run_assistant")
            foundIpcRunAssistant = true;
        if (name == "more_phi.parameters")
            foundMorePhiParameters = true;
        if (name == "more_phi.set_parameter")
            foundMorePhiSetParameter = true;
        if (name == "automation.history")
            foundAutomationHistory = true;
        if (name == "workflow.create")
            foundWorkflowCreate = true;
        if (name == "workflow.submit")
            foundWorkflowSubmit = true;
        if (name == "workflow.predict_next")
            foundWorkflowPredict = true;
        if (name == "permission.get_state")
            foundPermissionState = true;
        if (name == "context.get_session")
            foundContextSession = true;
        if (name == "memory.remember")
            foundMemoryRemember = true;
        if (name == "memory.update_outcome_feedback")
            foundMemoryUpdateOutcome = true;
        if (name == "events.list_recent")
            foundEventsRecent = true;
        if (name == "plugin_adapter.describe_capabilities")
            foundPluginAdapterCapabilities = true;

        if (name == "ozone_track_get_info")
            foundOzoneGetInfoUnderscore = true;
        if (name == "ozone_track_analyze")
            foundOzoneAnalyzeUnderscore = true;
    }

    REQUIRE(foundToolsCallAlias);
    REQUIRE(foundProfileAudit);
    REQUIRE(foundSemanticCanonical);
    REQUIRE(foundSemanticAlias);
    REQUIRE(foundPlanPreview);
    REQUIRE(foundRenderStatus);
    REQUIRE(foundOzoneGetInfo);
    REQUIRE(foundOzoneUpdateStatus);
    REQUIRE(foundOzoneAnalyze);
    REQUIRE(foundOzoneSearch);
    REQUIRE(foundOzoneAudit);
    REQUIRE(foundSpectrum);
    REQUIRE(foundStereoField);
    REQUIRE(foundOzoneGetInfoUnderscore);
    REQUIRE(foundOzoneAnalyzeUnderscore);
    REQUIRE(foundIpcStatus);
    REQUIRE(foundIpcSnapshot);
    REQUIRE(foundIpcRunAssistant);
    REQUIRE(foundMorePhiParameters);
    REQUIRE(foundMorePhiSetParameter);
    REQUIRE(foundHostedPluginSetParameter);
    REQUIRE(foundAutomationHistory);
    REQUIRE(foundWorkflowCreate);
    REQUIRE(foundWorkflowSubmit);
    REQUIRE(foundWorkflowPredict);
    REQUIRE(foundPermissionState);
    REQUIRE(foundContextSession);
    REQUIRE(foundMemoryRemember);
    REQUIRE(foundMemoryUpdateOutcome);
    REQUIRE(foundEventsRecent);
    REQUIRE(foundPluginAdapterCapabilities);
}

TEST_CASE("MCP More-Phi runtime parameter tools list, read, and write APVTS controls", "[mcp][more-phi]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    const auto listed = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("more_phi.parameters", {}, processor, identity).toStdString());
    REQUIRE(listed.is_array());
    REQUIRE_FALSE(listed.empty());

    bool foundMorphX = false;
    for (const auto& parameter : listed)
    {
        if (parameter["parameter_id"].get<std::string>() == "morphX")
        {
            foundMorphX = true;
            REQUIRE(parameter["name"].get<std::string>() == "Morph X");
            REQUIRE(parameter["value"].get<double>() == 0.5);
        }
    }
    REQUIRE(foundMorphX);

    juce::DynamicObject::Ptr getParams = new juce::DynamicObject();
    getParams->setProperty("parameter_id", "morphX");
    const auto before = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("more_phi.get_parameter", juce::var(getParams.get()), processor, identity).toStdString());
    REQUIRE(before["success"].get<bool>());
    REQUIRE(before["parameter_id"].get<std::string>() == "morphX");

    juce::DynamicObject::Ptr setParams = new juce::DynamicObject();
    setParams->setProperty("parameter_id", "morphX");
    setParams->setProperty("value", 0.25);
    const auto set = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("more_phi.set_parameter", juce::var(setParams.get()), processor, identity).toStdString());
    REQUIRE(set["success"].get<bool>());
    REQUIRE(set["parameter_id"].get<std::string>() == "morphX");
    REQUIRE(set["applied"].get<int>() == 1);
    REQUIRE(std::abs(processor.getAPVTS().getParameter("morphX")->getValue() - 0.25f) < 0.0001f);

    const auto after = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("more_phi.get_parameter", juce::var(getParams.get()), processor, identity).toStdString());
    REQUIRE(after["success"].get<bool>());
    REQUIRE(std::abs(after["value"].get<float>() - 0.25f) < 0.0001f);
}

TEST_CASE("MCP More-Phi runtime batch tool reports partial rejection", "[mcp][more-phi]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    juce::DynamicObject::Ptr valid = new juce::DynamicObject();
    valid->setProperty("parameter_id", "morphY");
    valid->setProperty("value", 0.75);

    juce::DynamicObject::Ptr invalid = new juce::DynamicObject();
    invalid->setProperty("parameter_id", "does_not_exist");
    invalid->setProperty("value", 0.5);

    juce::DynamicObject::Ptr root = new juce::DynamicObject();
    auto batch = juce::Array<juce::var>();
    batch.add(juce::var(valid.get()));
    batch.add(juce::var(invalid.get()));
    root->setProperty("parameters", batch);

    const auto response = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("more_phi.set_parameters", juce::var(root.get()), processor, identity).toStdString());

    REQUIRE_FALSE(response["success"].get<bool>());
    REQUIRE(response["applied"].get<int>() == 1);
    REQUIRE(response["rejected"].get<int>() == 1);
    REQUIRE(response["error"].get<std::string>() == "partial_rejected");
    REQUIRE(std::abs(processor.getAPVTS().getParameter("morphY")->getValue() - 0.75f) < 0.0001f);
}

TEST_CASE("MCP control-plane namespaces expose context, permissions, memory, and transaction history", "[mcp][automation]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;
    identity.instanceId = "automation-test-instance";

    const auto permissions = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.get_state", {}, processor, identity).toStdString());
    REQUIRE(permissions["success"].get<bool>());
    REQUIRE(permissions["permission"]["policy_model"].get<std::string>() == "system_enforced_dispatch_kernel_v1");

    const auto session = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("context.get_session", {}, processor, identity).toStdString());
    REQUIRE(session["success"].get<bool>());
    REQUIRE(session["session"]["instanceId"].get<std::string>() == "automation-test-instance");
    REQUIRE(session["session"]["context_metadata"]["provider"].get<std::string>() == "SessionContextProvider");

    juce::DynamicObject::Ptr memoryParams = new juce::DynamicObject();
    memoryParams->setProperty("scope", "global");
    memoryParams->setProperty("kind", "preference");
    memoryParams->setProperty("text", "Prefer conservative automation changes");
    const auto remembered = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("memory.remember", juce::var(memoryParams.get()), processor, identity).toStdString());
    REQUIRE(remembered["success"].get<bool>());

    juce::DynamicObject::Ptr morphParams = new juce::DynamicObject();
    morphParams->setProperty("x", 0.35);
    const auto write = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("set_morph_position", juce::var(morphParams.get()), processor, identity).toStdString());
    REQUIRE(write["success"].get<bool>());
    REQUIRE(write.contains("transaction_id"));
    REQUIRE(write["automation"]["risk"].get<std::string>() == "low_write");

    const auto history = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("automation.history", {}, processor, identity).toStdString());
    REQUIRE(history["success"].get<bool>());
    REQUIRE(history["transactions"].is_array());

    bool foundMorphTransaction = false;
    for (const auto& transaction : history["transactions"])
        if (transaction["id"].get<std::string>() == write["transaction_id"].get<std::string>())
            foundMorphTransaction = true;
    REQUIRE(foundMorphTransaction);
}

TEST_CASE("MCP workflow.submit executes steps through permissioned transactions", "[mcp][automation][workflow]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;
    identity.instanceId = "workflow-submit-test-instance";

    const nlohmann::json submitArgs{
        {"user_intent", "move the morph cursor through a workflow"},
        {"steps", nlohmann::json::array({
            {
                {"id", "move"},
                {"toolName", "set_morph_position"},
                {"params", {{"x", 0.42}}},
                {"maxRetries", 0}
            }
        })}
    };

    const auto submitted = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("workflow.submit", toVar(submitArgs), processor, identity).toStdString());
    REQUIRE(submitted["success"].get<bool>());
    const auto workflowId = submitted["workflow_run"]["id"].get<std::string>();
    REQUIRE_FALSE(workflowId.empty());

    const auto executed = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("workflow.execute",
            toVar(nlohmann::json{{"workflow_run_id", workflowId}}), processor, identity).toStdString());
    REQUIRE(executed["success"].get<bool>());
    REQUIRE(executed["workflow_run"]["state"].get<std::string>() == "completed");
    REQUIRE(std::abs(processor.getMorphX() - 0.42f) < 0.0001f);

    const auto history = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("automation.history",
            toVar(nlohmann::json{{"limit", 50}, {"workflow_run_id", workflowId}}), processor, identity).toStdString());
    REQUIRE(history["success"].get<bool>());
    REQUIRE(history["workflow_run_id"].get<std::string>() == workflowId);

    bool foundStepTransaction = false;
    std::string stepTransactionId;
    for (const auto& transaction : history["transactions"])
    {
        const bool isStepTransaction = transaction["workflowRunId"].get<std::string>() == workflowId
            && transaction["workflowStepId"].get<std::string>() == "move"
            && transaction["toolName"].get<std::string>() == "set_morph_position";
        foundStepTransaction = foundStepTransaction || isStepTransaction;
        if (isStepTransaction)
            stepTransactionId = transaction["id"].get<std::string>();
    }
    REQUIRE(foundStepTransaction);
    REQUIRE_FALSE(stepTransactionId.empty());

    const auto outcomes = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("memory.list_outcomes",
            toVar(nlohmann::json{{"workflow_run_id", workflowId}, {"limit", 10}}), processor, identity).toStdString());
    REQUIRE(outcomes["success"].get<bool>());
    REQUIRE(outcomes["outcomes"].size() == 1);
    REQUIRE(outcomes["outcomes"][0]["content"]["actionId"].get<std::string>() == stepTransactionId);
    REQUIRE(outcomes["outcomes"][0]["content"]["feedbackStatus"].get<std::string>() == "unreviewed");

    const auto updated = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("memory.update_outcome_feedback",
            toVar(nlohmann::json{
                {"action_id", stepTransactionId},
                {"feedback_status", "too_much"},
                {"user_feedback", "the morph move was too aggressive"}
            }), processor, identity).toStdString());
    REQUIRE(updated["success"].get<bool>());
    REQUIRE(updated["outcome"]["actionId"].get<std::string>() == stepTransactionId);
    REQUIRE(updated["outcome"]["feedbackStatus"].get<std::string>() == "too_much");
    REQUIRE(updated["outcome"]["userFeedback"].get<std::string>() == "the morph move was too aggressive");
    REQUIRE_FALSE(updated["outcome"]["userAccepted"].get<bool>());

    const auto updatedOutcomes = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("memory.list_outcomes",
            toVar(nlohmann::json{{"workflow_run_id", workflowId}, {"limit", 10}}), processor, identity).toStdString());
    REQUIRE(updatedOutcomes["success"].get<bool>());
    REQUIRE(updatedOutcomes["outcomes"].size() == 1);
    REQUIRE(updatedOutcomes["outcomes"][0]["id"].get<std::string>() == outcomes["outcomes"][0]["id"].get<std::string>());
    REQUIRE(updatedOutcomes["outcomes"][0]["content"]["feedbackStatus"].get<std::string>() == "too_much");

    const auto prediction = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("workflow.predict_next",
            toVar(nlohmann::json{{"workflow_run_id", workflowId}, {"user_intent", "continue safely"}}), processor, identity).toStdString());
    REQUIRE(prediction["success"].get<bool>());
    REQUIRE(prediction["read_only"].get<bool>());
    REQUIRE(prediction["candidate_next_actions"].is_array());
    REQUIRE_FALSE(prediction["candidate_next_actions"].empty());
    REQUIRE(prediction["memory_evidence"].is_array());
    REQUIRE(prediction["memory_evidence"].size() == 1);
    REQUIRE(prediction["memory_evidence"][0]["action_id"].get<std::string>() == stepTransactionId);
    REQUIRE(prediction["memory_evidence"][0]["advisory"].get<bool>());
    REQUIRE_FALSE(prediction["memory_policy"]["can_grant_permission"].get<bool>());
}

TEST_CASE("MCP PermissionPolicy blocks high-impact hosted plugin load with approval request", "[mcp][automation][permission]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    juce::DynamicObject::Ptr autonomy = new juce::DynamicObject();
    autonomy->setProperty("level", "assist");
    const auto setPolicy = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.set_autonomy", juce::var(autonomy.get()), processor, identity).toStdString());
    REQUIRE(setPolicy["success"].get<bool>());

    juce::DynamicObject::Ptr loadParams = new juce::DynamicObject();
    loadParams->setProperty("path", "C:/definitely-missing-plugin.vst3");
    const auto blocked = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("hosted_plugin.load", juce::var(loadParams.get()), processor, identity).toStdString());

    REQUIRE_FALSE(blocked["success"].get<bool>());
    REQUIRE(blocked["error"].get<std::string>() == "approval_required");
    REQUIRE(blocked["approval_required"].get<bool>());
    REQUIRE(blocked["risk"].get<std::string>() == "high_impact");
    REQUIRE(blocked["approval_request"]["toolName"].get<std::string>() == "hosted_plugin.load");
}

TEST_CASE("MCP permission approval decisions are auditable", "[mcp][automation][permission]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    const auto setPolicy = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.set_autonomy",
            toVar(nlohmann::json{{"level", "assist"}}), processor, identity).toStdString());
    REQUIRE(setPolicy["success"].get<bool>());

    const auto blocked = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("hosted_plugin.load",
            toVar(nlohmann::json{{"path", "C:/definitely-missing-plugin.vst3"}}),
            processor,
            identity).toStdString());

    REQUIRE_FALSE(blocked["success"].get<bool>());
    REQUIRE(blocked["approval_required"].get<bool>());
    const auto approvalId = blocked["approval_request"]["id"].get<std::string>();
    REQUIRE_FALSE(approvalId.empty());

    const auto approvalsBefore = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.list_approvals", {}, processor, identity).toStdString());
    REQUIRE(approvalsBefore["success"].get<bool>());

    bool foundPending = false;
    for (const auto& approval : approvalsBefore["approvals"])
    {
        foundPending = foundPending
            || (approval.value("id", std::string{}) == approvalId
                && approval.value("status", std::string{}) == "pending");
    }
    REQUIRE(foundPending);

    const auto approved = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.approve",
            toVar(nlohmann::json{{"approval_id", approvalId}}),
            processor,
            identity).toStdString());

    REQUIRE(approved["success"].get<bool>());
    REQUIRE(approved.contains("transaction_id"));
    REQUIRE(approved["automation"]["risk"].get<std::string>() == "low_write");

    const auto history = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("automation.history",
            toVar(nlohmann::json{{"limit", 50}}), processor, identity).toStdString());
    REQUIRE(history["success"].get<bool>());

    bool foundApproveTransaction = false;
    for (const auto& transaction : history["transactions"])
    {
        foundApproveTransaction = foundApproveTransaction
            || (transaction.value("id", std::string{}) == approved["transaction_id"].get<std::string>()
                && transaction.value("toolName", std::string{}) == "permission.approve");
    }
    REQUIRE(foundApproveTransaction);

    const auto approvalsAfter = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.list_approvals", {}, processor, identity).toStdString());
    REQUIRE(approvalsAfter["success"].get<bool>());

    bool foundApproved = false;
    for (const auto& approval : approvalsAfter["approvals"])
    {
        foundApproved = foundApproved
            || (approval.value("id", std::string{}) == approvalId
                && approval.value("status", std::string{}) == "approved");
    }
    REQUIRE(foundApproved);
}

TEST_CASE("MCP approval requests include predicted diffs for reversible controls", "[mcp][automation][permission]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    auto manualPolicy = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.set_autonomy",
            toVar(nlohmann::json{{"level", "manual"}}), processor, identity).toStdString());
    if (manualPolicy.value("approval_required", false))
    {
        const auto approvalId = manualPolicy["approval_request"]["id"].get<std::string>();
        const auto approved = nlohmann::json::parse(
            more_phi::MCPToolHandler::handle("permission.approve",
                toVar(nlohmann::json{{"approval_id", approvalId}}), processor, identity).toStdString());
        REQUIRE(approved["success"].get<bool>());
        manualPolicy = nlohmann::json::parse(
            more_phi::MCPToolHandler::handle("permission.set_autonomy",
                toVar(nlohmann::json{{"level", "manual"}, {"approval_id", approvalId}}), processor, identity).toStdString());
    }
    REQUIRE(manualPolicy["success"].get<bool>());

    const auto blocked = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("set_morph_position",
            toVar(nlohmann::json{{"x", 0.61}}), processor, identity).toStdString());

    REQUIRE_FALSE(blocked["success"].get<bool>());
    REQUIRE(blocked["approval_required"].get<bool>());
    REQUIRE(blocked["approval_request"]["predictedDiff"]["approval_preview"].get<bool>());
    REQUIRE(blocked["approval_request"]["predictedDiff"]["diffs"].is_array());
    REQUIRE_FALSE(blocked["approval_request"]["predictedDiff"]["diffs"].empty());
    REQUIRE(blocked["approval_request"]["predictedDiff"]["diffs"][0]["control"].get<std::string>() == "morph_x");
    REQUIRE(std::abs(blocked["approval_request"]["predictedDiff"]["diffs"][0]["after"].get<double>() - 0.61) < 0.0001);

    const auto resetAttempt = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("permission.set_autonomy",
            toVar(nlohmann::json{{"level", "assist"}}), processor, identity).toStdString());
    if (resetAttempt.value("approval_required", false))
    {
        const auto approvalId = resetAttempt["approval_request"]["id"].get<std::string>();
        const auto approved = nlohmann::json::parse(
            more_phi::MCPToolHandler::handle("permission.approve",
                toVar(nlohmann::json{{"approval_id", approvalId}}), processor, identity).toStdString());
        REQUIRE(approved["success"].get<bool>());

        const auto reset = nlohmann::json::parse(
            more_phi::MCPToolHandler::handle("permission.set_autonomy",
                toVar(nlohmann::json{{"level", "assist"}, {"approval_id", approvalId}}), processor, identity).toStdString());
        REQUIRE(reset["success"].get<bool>());
    }
}

TEST_CASE("VST3 MCP handler exposes IPC assistant status and write gate", "[mcp][ipc]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    const auto status = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("izotope_ipc_status", {}, processor, identity).toStdString());
    REQUIRE(status.contains("attached"));
    REQUIRE_FALSE(status["attached"].get<bool>());

    juce::DynamicObject::Ptr runParams = new juce::DynamicObject();
    runParams->setProperty("allow_unsafe_write", false);
    const auto blocked = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("ozone_run_assistant", juce::var(runParams.get()), processor, identity).toStdString());

    REQUIRE_FALSE(blocked["success"].get<bool>());
    REQUIRE(blocked["error"].get<std::string>() == "approval_required");
    REQUIRE(blocked["risk"].get<std::string>() == "external");
}


TEST_CASE("MCP Ozone audit discovers and applies hosted parameter map", "[mcp][ozone]")
{
    more_phi::MorePhiProcessor processor;
    std::vector<more_phi::ParameterBridge::ParameterDescriptor> descriptors;

    auto addDescriptor = [&descriptors](const char* name)
    {
        more_phi::ParameterBridge::ParameterDescriptor descriptor;
        descriptor.index = static_cast<int>(descriptors.size());
        descriptor.name = name;
        descriptors.push_back(descriptor);
    };

    addDescriptor("EQ Band 1 Frequency");
    addDescriptor("EQ Band 1 Gain");
    addDescriptor("EQ Band 1 Q");
    addDescriptor("EQ Band 1 Type");
    addDescriptor("EQ Band 1 Enabled");
    addDescriptor("Dynamics Threshold");
    addDescriptor("Dynamics Ratio");
    addDescriptor("Dynamics Attack");
    addDescriptor("Dynamics Release");
    addDescriptor("Imager Sub Width");
    addDescriptor("Imager Low Width");
    addDescriptor("Imager Mid Width");
    addDescriptor("Imager High Width");
    addDescriptor("Maximizer Output Level");
    addDescriptor("Maximizer Ceiling");

    processor.getParameterBridge().setParameterDescriptorsForTesting(std::move(descriptors));

    more_phi::InstanceIdentity identity;
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    params->setProperty("apply", true);

    const auto audit = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("ozone.audit_parameters", juce::var(params.get()), processor, identity).toStdString());

    REQUIRE(audit["success"].get<bool>());
    REQUIRE(audit["applied"].get<bool>());
    REQUIRE(audit["matched_count"].get<int>() == 15);
    REQUIRE(audit["ozone_applicator_active"].get<bool>());
    REQUIRE(audit["eq"][0]["frequency"].get<int>() == 0);
    REQUIRE(audit["dynamics"]["threshold"].get<int>() == 5);
    REQUIRE(audit["imager"]["width"].size() == 4);
    REQUIRE(audit["maximizer"]["ceiling"].get<int>() == 14);
}

TEST_CASE("MCP semantic map describes hosted parameters as safe LLM controls", "[mcp][semantic]")
{
    more_phi::MorePhiProcessor processor;
    std::vector<more_phi::ParameterBridge::ParameterDescriptor> descriptors{
        makeDescriptor("EQ Band 1 Frequency", "280 Hz", "Hz"),
        makeDescriptor("EQ Band 1 Gain", "-1.5 dB", "dB"),
        makeDescriptor("EQ Band 1 Q", "0.90"),
        makeDescriptor("Dynamics Threshold", "-18 dB", "dB"),
        makeDescriptor("Dynamics Ratio", "2.0:1"),
        makeDescriptor("Dynamics Attack", "30 ms", "ms"),
        makeDescriptor("Dynamics Release", "120 ms", "ms"),
        makeDescriptor("Maximizer Ceiling", "-1.0 dBTP", "dBTP"),
        makeDescriptor("Maximizer Output Level", "-0.5 dB", "dB"),
        makeDescriptor("Imager Low Width", "1.00"),
        makeDescriptor("Imager High Width", "1.15"),
        makeDescriptor("Saturation Drive", "8%"),
        makeDescriptor("Bypass", "Off", "", true, true),
        makeDescriptor("Preset Randomize", "Off", "", true, true)
    };
    assignDescriptorIndices(descriptors);
    processor.getParameterBridge().setParameterDescriptorsForTesting(std::move(descriptors));

    more_phi::InstanceIdentity identity;
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    params->setProperty("include_raw_parameters", true);
    params->setProperty("max_controls", 32);

    const auto semantic = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle(
            "plugin_profile.describe_semantic_map", juce::var(params.get()), processor, identity).toStdString());

    REQUIRE(semantic["success"].get<bool>());
    REQUIRE(semantic["schema_version"].get<int>() == 1);
    REQUIRE(semantic["plugin_id"].get<std::string>() == "current");
    REQUIRE(semantic["profile_id"].get<std::string>() == "current_14_parameters");
    REQUIRE(semantic["detected_type"].get<std::string>() == "mastering_suite");
    REQUIRE(semantic["confidence"].get<float>() > 0.8f);
    REQUIRE(jsonArrayContainsString(semantic["supports"], "eq"));
    REQUIRE(jsonArrayContainsString(semantic["supports"], "compressor"));
    REQUIRE(jsonArrayContainsString(semantic["supports"], "limiter"));
    REQUIRE(jsonArrayContainsString(semantic["supports"], "stereo_imager"));
    REQUIRE(jsonArrayContainsString(semantic["supports"], "saturation"));

    const auto* eqFreq = findSemanticControl(semantic["safe_controls"], "band_1_frequency");
    REQUIRE(eqFreq != nullptr);
    REQUIRE((*eqFreq)["param_id"].get<int>() == 0);
    REQUIRE((*eqFreq)["safe_range"].get<std::string>() == "20 Hz to 20000 Hz");

    const auto* eqGain = findSemanticControl(semantic["safe_controls"], "band_1_gain");
    REQUIRE(eqGain != nullptr);
    REQUIRE((*eqGain)["safe_range"].get<std::string>() == "-6 dB to +3 dB");

    REQUIRE(findSemanticControl(semantic["safe_controls"], "band_1_q") != nullptr);
    REQUIRE(findSemanticControl(semantic["safe_controls"], "compressor_threshold") != nullptr);

    const auto* ceiling = findSemanticControl(semantic["caution_controls"], "limiter_ceiling");
    REQUIRE(ceiling != nullptr);
    REQUIRE((*ceiling)["safe_range"].get<std::string>() == "-2.0 dBTP to -0.8 dBTP");

    const auto* lowWidth = findSemanticControl(semantic["caution_controls"], "imager_low_width");
    REQUIRE(lowWidth != nullptr);
    REQUIRE((*lowWidth)["safe_range"].get<std::string>() == "max 1.0");
    REQUIRE(findSemanticControl(semantic["caution_controls"], "saturation_drive") != nullptr);

    REQUIRE(findSemanticControl(semantic["blocked_controls"], "control_bypass") != nullptr);
    REQUIRE(findSemanticControl(semantic["blocked_controls"], "control_preset_randomize") != nullptr);
    REQUIRE(semantic.contains("raw_parameters"));
    REQUIRE(semantic["raw_parameters"].size() == 14);
}

TEST_CASE("MCP semantic map alias validates current plugin id and empty parameter state", "[mcp][semantic]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    const auto empty = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("describe_plugin_semantic_map", {}, processor, identity).toStdString());
    REQUIRE_FALSE(empty["success"].get<bool>());
    REQUIRE(empty["error"].get<std::string>() == "no_hosted_parameters");

    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    params->setProperty("plugin_id", "other");
    const auto invalid = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle(
            "describe_plugin_semantic_map", juce::var(params.get()), processor, identity).toStdString());
    REQUIRE_FALSE(invalid["success"].get<bool>());
    REQUIRE(invalid["error"].get<std::string>() == "invalid_plugin_id");
}

TEST_CASE("MCP analysis tools expose spectrum and stereo field snapshots", "[mcp][analysis]")
{
    more_phi::MorePhiProcessor processor;
    processor.getAutoMasteringEngine().prepare(48000.0, 512);
    processor.getAutoMasteringEngine().setActive(true);

    juce::AudioBuffer<float> buffer(2, 512);
    for (int block = 0; block < 48; ++block)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const int sampleIndex = block * buffer.getNumSamples() + i;
            const float sample = 0.2f * std::sin(2.0f * juce::MathConstants<float>::pi
                * 1000.0f * static_cast<float>(sampleIndex) / 48000.0f);
            buffer.setSample(0, i, sample * 1.2f);
            buffer.setSample(1, i, sample * 0.8f);
        }

        processor.getAutoMasteringEngine().processBlock(buffer);
    }

    more_phi::InstanceIdentity identity;
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    params->setProperty("resolution", 32);

    const auto spectrum = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("analysis.get_spectrum", juce::var(params.get()), processor, identity).toStdString());
    REQUIRE(spectrum["success"].get<bool>());
    REQUIRE(spectrum["resolution"].get<int>() == 32);
    REQUIRE(spectrum["bin_count"].get<int>() == 32);
    REQUIRE(spectrum["magnitude_db"].is_array());
    REQUIRE(spectrum["magnitude_db"].size() == 32);
    REQUIRE(spectrum["spectral_centroid_hz"].get<double>() > 500.0);
    REQUIRE(spectrum["bin_width_hz"].get<double>() > 0.0);
    REQUIRE(spectrum["channel_mode"].get<std::string>() == "mono_sum");
    REQUIRE(spectrum["method"].get<std::string>() == "hann_window_fft_mono_sum");
    REQUIRE(spectrum["analysis_metadata"]["method"].get<std::string>() == "deterministic_dsp_metering");
    REQUIRE(spectrum["analysis_metadata"]["measurement_state"].get<std::string>() == "latest_available_snapshot");
    REQUIRE(spectrum["analysis_metadata"]["algorithm_ids"]["spectrum"].get<std::string>() == "hann_window_fft_mono_sum");
    REQUIRE(spectrum["warnings"].is_array());
    REQUIRE(jsonArrayContainsString(
        spectrum["warnings"],
        "spectrum_uses_mono_sum_downmix; anti_phase_stereo_can_cancel_in_this_view"));

    const auto stereo = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("analysis.get_stereo_field", {}, processor, identity).toStdString());
    REQUIRE(stereo["success"].get<bool>());
    REQUIRE(stereo["method"].get<std::string>() == "mid_side_energy_analysis");
    REQUIRE(stereo["algorithm"].get<std::string>() == "mid_side_transform_plus_second_order_butterworth_band_split");
    REQUIRE(stereo["bands"].is_array());
    REQUIRE(stereo["bands"].size() == 4);
    REQUIRE(stereo["band_definitions"].is_array());
    REQUIRE(stereo["band_definitions"].size() == 4);
    REQUIRE(stereo["stereo_width"].get<double>() > 0.0);
    REQUIRE(stereo["mid_side_width"].get<double>() > 0.0);
    REQUIRE(stereo["bands"][0].contains("mid_side_correlation"));
    REQUIRE(stereo["bands"][0].contains("side_to_mid_energy_ratio"));
    REQUIRE(stereo["analysis_metadata"]["algorithm_ids"]["stereo_field"].get<std::string>() == "mid_side_energy_butterworth_bands");
    REQUIRE(jsonArrayContainsString(
        stereo["warnings"],
        "stereo_field_is_mid_side_energy_analysis_not_perceptual_width_prediction"));
}

TEST_CASE("MCP analysis summary reports deterministic methodology and model status", "[mcp][analysis]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    const auto summary = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("analysis.get_summary", {}, processor, identity).toStdString());

    REQUIRE(summary["success"].get<bool>());
    REQUIRE(summary["schema_version"].get<int>() == 1);
    REQUIRE(summary["measurements"].is_object());
    REQUIRE(summary["true_peak_method"].get<std::string>() == "4x_polyphase_fir_estimate");
    REQUIRE(summary["loudness_method"].get<std::string>() == "lightweight_bs1770_style_rolling_estimate");
    REQUIRE(summary["analysis_metadata"]["methodology"].get<std::string>() == "deterministic_dsp");
    REQUIRE(summary["analysis_metadata"]["method"].get<std::string>() == "deterministic_dsp_metering");
    REQUIRE(summary["analysis_metadata"]["metering_standard_claim"].get<std::string>() == "lightweight_bs1770_style_estimates");
    REQUIRE(summary["analysis_metadata"]["algorithm_ids"]["loudness"].get<std::string>() == "lightweight_bs1770_style_rolling_estimate");
    REQUIRE(summary["analysis_metadata"]["algorithm_ids"]["true_peak"].get<std::string>() == "4x_polyphase_fir_estimate");
    REQUIRE(summary["model_status"]["genre_classifier_loaded"].get<bool>() == false);
    REQUIRE(summary["model_status"]["genre_classifier_status"].get<std::string>() == "default_fallback");
    REQUIRE(summary["model_status"]["genre_classifier_inference"].get<std::string>() == "unavailable");
    REQUIRE(summary["warnings"].is_array());
    REQUIRE(jsonArrayContainsString(
        summary["warnings"],
        "lufs_values_are_rolling_available_history_estimates_not_external_lab_certification"));
}

TEST_CASE("sonicmaster_decision separates raw model telemetry from projected engine mapping",
          "[mcp][sonicmaster]")
{
    StubSonicMasterSource source;
    more_phi::MorePhiProcessor processor;
    processor.getAutoMasteringEngine().prepare(48000.0, 512, false);

    auto& sonicMaster = processor.getSonicMasterEngine();
    sonicMaster.setInferenceSource(&source);
    sonicMaster.prepare(48000.0, 512);
    feedSonicMasterWindow(sonicMaster, 48000.0);

    more_phi::InstanceIdentity identity;
    const auto response = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle(
            "sonicmaster_decision",
            toVar(nlohmann::json{{"target_lufs", -14.0}}),
            processor,
            identity).toStdString());

    sonicMaster.release();
    processor.getAutoMasteringEngine().reset();

    REQUIRE(response["success"].get<bool>());
    REQUIRE(response["applied"].get<bool>() == false);
    REQUIRE(response["response_schema_version"].get<int>() == 2);
    REQUIRE(response["raw_model_decision"]["field_semantics"].get<std::string>()
            == "raw_model_telemetry_not_applied_parameters");
    REQUIRE(response["projected_plan"]["applied_mask"]["limiter"].get<bool>() == false);
    REQUIRE(response["actual_engine_mapping"]["limiter"]["applied_if_confirmed"].get<bool>() == false);

    const auto rawEqDb = response["raw_model_decision"]["eq_bands"][0]["gainDb"].get<double>();
    const auto projectedEq = response["projected_plan"]["eq_normalized"][0].get<double>();
    const auto mappedEqDb = response["actual_engine_mapping"]["eq_bands"][0]["gainDb"].get<double>();
    CHECK(std::abs(rawEqDb - 12.0) < 1.0e-6);
    CHECK(std::abs(projectedEq - 0.15) < 1.0e-6);
    CHECK(std::abs(mappedEqDb - 1.8) < 1.0e-5);

    CHECK(std::abs(response["raw_model_decision"]["compressor_bands"][0]["attackMs"].get<double>() - 1.0) < 1.0e-6);
    // AUDIT-FIX: engineMapping now reads the model's decoded compParams
    // sidecar directly, so attackMs is a plain number (not a nested object
    // with a "source" key). The stub returns attackMs=1.0 for band 0.
    CHECK(std::abs(response["actual_engine_mapping"]["dynamics_bands"][0]["attackMs"].get<double>() - 1.0) < 1.0e-6);
    REQUIRE(jsonArrayContainsString(
        response["actual_engine_mapping"]["dynamics_bands"][0]["direct_model_controls"],
        "thresholdDb"));
    // AUDIT-FIX: all 6 compressor params are now direct_model_controls;
    // raw_telemetry_only_controls was removed.
    REQUIRE(jsonArrayContainsString(
        response["actual_engine_mapping"]["dynamics_bands"][0]["direct_model_controls"],
        "attackMs"));
    REQUIRE(jsonArrayContainsString(
        response["warnings"],
        "decision_contains_raw_model_telemetry_not_applied_parameters"));
    // AUDIT-FIX: the warning "compressor_attack_release_makeup_knee_are_raw_telemetry"
    // was removed because the engineMapping now reports actual model values from
    // the compParams sidecar, not stale DSP state.

    // Legacy aliases remain available for older clients.
    REQUIRE(response["decision"]["eq_bands"].is_array());
    REQUIRE(response["plan_eq_normalized"].is_array());
}

TEST_CASE("MCP capture window reports no samples before analysis tap has data", "[mcp][analysis][MeterWindow]")
{
    more_phi::MorePhiProcessor processor;
    processor.getAutoMasteringEngine().prepare(48000.0, 512, false);

    more_phi::InstanceIdentity identity;
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    params->setProperty("window_seconds", 3.0);

    const auto captured = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("analysis.capture_window", juce::var(params.get()), processor, identity).toStdString());

    REQUIRE_FALSE(captured["success"].get<bool>());
    REQUIRE(captured["error"].get<std::string>() == "no_window_samples");
    REQUIRE(captured["requested_window_seconds"].get<double>() == 3.0);
    REQUIRE(captured["sample_count"].get<int>() == 0);
    REQUIRE(captured["current_snapshot"].is_object());
}

TEST_CASE("MCP capture window returns accumulated rolling meter statistics", "[mcp][analysis][MeterWindow]")
{
    more_phi::MorePhiProcessor processor;
    processor.getAutoMasteringEngine().prepare(48000.0, 512, false);

    juce::AudioBuffer<float> buffer(2, 512);
    for (int block = 0; block < 24; ++block)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const int sampleIndex = block * buffer.getNumSamples() + i;
            const float sample = 0.15f * std::sin(2.0f * juce::MathConstants<float>::pi
                * 440.0f * static_cast<float>(sampleIndex) / 48000.0f);
            buffer.setSample(0, i, sample);
            buffer.setSample(1, i, sample);
        }

        processor.getAutoMasteringEngine().analyzeBlock(buffer);
    }

    more_phi::InstanceIdentity identity;
    juce::DynamicObject::Ptr params = new juce::DynamicObject();
    params->setProperty("window_seconds", 3.0);

    const auto captured = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("analysis.capture_window", juce::var(params.get()), processor, identity).toStdString());

    REQUIRE(captured["success"].get<bool>());
    REQUIRE(captured["sample_count"].get<int>() >= 2);
    REQUIRE(captured["window_statistics"].is_object());
    REQUIRE(captured["window_statistics"]["rms"]["mean"].get<double>() > 0.01);
    REQUIRE(captured["analysis_metadata"]["data_scope"].get<std::string>() == "rolling_window");
    REQUIRE(captured["analysis_metadata"]["measurement_state"].get<std::string>() == "rolling_available_history");
    REQUIRE(captured["analysis_metadata"]["algorithm_ids"]["window_statistics"].get<std::string>() == "rolling_min_max_mean_p10_p50_p90");
}

TEST_CASE("MCP dry-run mastering candidates can be selected and applied", "[mcp][mastering]")
{
    more_phi::MorePhiProcessor processor;
    more_phi::InstanceIdentity identity;

    juce::DynamicObject::Ptr renderParams = new juce::DynamicObject();
    renderParams->setProperty("dry_run", true);
    renderParams->setProperty("candidate_count", 2);
    renderParams->setProperty("dynamic_range", 5.0f);
    renderParams->setProperty("spectral_tilt", -1.0f);

    const auto batch = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("mastering.render_batch", juce::var(renderParams.get()), processor, identity).toStdString());

    REQUIRE(batch["success"].get<bool>());
    REQUIRE(batch["candidates"].is_array());
    REQUIRE(batch["candidates"].size() == 2);
    REQUIRE(batch["planner_type"].get<std::string>() == "heuristic_rule_engine");
    REQUIRE(batch["planner_metadata"]["recommendation_type"].get<std::string>() == "heuristic_rule_engine");
    REQUIRE(batch["planner_metadata"]["confidence"].is_null());
    REQUIRE_FALSE(batch["score_available"].get<bool>());
    REQUIRE(batch["score_basis"].get<std::string>() == "not_scored_without_audio_render");
    REQUIRE(batch["candidates"][0]["recommendation_type"].get<std::string>() == "heuristic_rule_engine");
    REQUIRE(batch["candidates"][0]["planner_metadata"]["confidence"].is_null());
    REQUIRE(batch["candidates"][0]["measured_inputs"].is_object());
    REQUIRE(batch["candidates"][0]["rules_applied"].is_array());
    REQUIRE(batch["candidates"][0]["rules_applied"].size() == 5);
    REQUIRE(batch["candidates"][0]["rules_applied"][0]["rule_id"].get<std::string>() == "dynamics_lra_thresholds_v1");
    REQUIRE_FALSE(batch["candidates"][0]["score_available"].get<bool>());
    REQUIRE(batch["candidates"][0]["score_basis"].get<std::string>() == "not_scored_without_audio_render");

    const auto candidateId = batch["candidates"][0]["id"].get<std::string>();
    juce::DynamicObject::Ptr selectParams = new juce::DynamicObject();
    selectParams->setProperty("candidate_id", juce::String(candidateId));

    const auto selected = nlohmann::json::parse(
        more_phi::MCPToolHandler::handle("mastering.select_candidate", juce::var(selectParams.get()), processor, identity).toStdString());

    REQUIRE(selected["success"].get<bool>());
    REQUIRE(selected["selected"].get<bool>());
    REQUIRE(selected["applied"].get<bool>());
    REQUIRE(selected["candidate_id"].get<std::string>() == candidateId);
    REQUIRE(selected["valid"].get<bool>());
    REQUIRE_FALSE(selected["score_available"].get<bool>());
    REQUIRE(selected["score_basis"].get<std::string>() == "not_scored_without_audio_render");
    REQUIRE(selected["measured_inputs"].is_object());
    REQUIRE(selected["rules_applied"].is_array());
    REQUIRE(selected["rules_applied"].size() == 5);
    REQUIRE(selected["rules_applied"][0]["rule_id"].get<std::string>() == "dynamics_lra_thresholds_v1");
    REQUIRE(selected["planner_metadata"]["confidence"].is_null());
    REQUIRE(selected["recommendation_type"].get<std::string>() == "heuristic_rule_engine");
    REQUIRE(processor.getAutoMasteringEngine().getChainPlanner().getLastPlan().valid);
}

TEST_CASE("Rendered mastering candidate score is based on render sanity metrics", "[mcp][mastering]")
{
    const auto sane = more_phi::scoreRenderedMasteringCandidate({ true, -1.2f, -18.0f, false, false });
    const auto clipped = more_phi::scoreRenderedMasteringCandidate({ true, 0.2f, -18.0f, false, true });
    const auto silent = more_phi::scoreRenderedMasteringCandidate({ true, -90.0f, -90.0f, true, false });
    const auto failed = more_phi::scoreRenderedMasteringCandidate({ false, -1.2f, -18.0f, false, false });

    REQUIRE(sane > 0.9f);
    REQUIRE(clipped < sane);
    REQUIRE(silent < sane);
    REQUIRE(failed == 0.0f);
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

// ── Multi-agent orchestration: agents.* risk classification ──────────────────
TEST_CASE("PermissionKernel classifies agents.* tools", "[agents][mcp][permissions]")
{
    more_phi::PermissionKernel kernel;
    using R = more_phi::RiskLevel;
    REQUIRE(kernel.classifyTool("agents.list", {})              == R::ReadOnly);
    REQUIRE(kernel.classifyTool("agents.run_goal", {})          == R::LowWrite);
    REQUIRE(kernel.classifyTool("agents.run_task", {})          == R::MediumWrite);
    REQUIRE(kernel.classifyTool("agents.run_status", {})        == R::ReadOnly);
    REQUIRE(kernel.classifyTool("agents.run_cancel", {})        == R::LowWrite);
    REQUIRE(kernel.classifyTool("agents.blackboard.recent", {}) == R::ReadOnly);
    REQUIRE(kernel.classifyTool("agents.set_autonomy", {})      == R::HighImpact);
}
