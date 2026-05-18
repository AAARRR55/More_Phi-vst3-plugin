/*
 * More-Phi - UI/AIChatPanel.cpp
 */
#include "AIChatPanel.h"
#include "LLMSettingsDialog.h"
#include "AI/MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <string>

namespace more_phi {

namespace {

enum class ToolGroupId : std::size_t
{
    HostedPlugin = 0,
    MorePhiRuntime,
    Snapshots,
    Analysis,
    Mastering,
    PluginProfiles,
    Izotope,
    Dataset,
    ParameterAi,
    Other,
    Count
};

struct ToolGroupSummary
{
    const char* label;
    int count;
};

std::array<ToolGroupSummary, static_cast<std::size_t>(ToolGroupId::Count)> makeEmptyToolGroups()
{
    return {{
        {"Hosted plugin controls", 0},
        {"More-Phi runtime controls", 0},
        {"Snapshots and morphing", 0},
        {"Analysis and metering", 0},
        {"Mastering workflow", 0},
        {"Plugin profile and safety", 0},
        {"iZotope/Ozone IPC", 0},
        {"Dataset generation", 0},
        {"Parameter AI utilities", 0},
        {"Diagnostics and other tools", 0},
    }};
}

ToolGroupId classifyToolName(const juce::String& name)
{
    if (name.startsWith("hosted_plugin.") || name == "get_plugin_info"
        || name == "list_parameters" || name == "get_parameter"
        || name == "set_parameter" || name == "set_parameters_batch")
    {
        return ToolGroupId::HostedPlugin;
    }

    if (name.startsWith("more_phi.") || name == "get_morph_state"
        || name == "set_morph_position")
    {
        return ToolGroupId::MorePhiRuntime;
    }

    if (name.containsIgnoreCase("snapshot") || name.containsIgnoreCase("morph"))
        return ToolGroupId::Snapshots;

    if (name.startsWith("analysis."))
        return ToolGroupId::Analysis;

    if (name.startsWith("mastering.") || name == "get_mastering_state"
        || name == "apply_mastering_plan")
    {
        return ToolGroupId::Mastering;
    }

    if (name.startsWith("plugin_profile.") || name == "describe_plugin_semantic_map")
        return ToolGroupId::PluginProfiles;

    if (name.startsWith("izotope_ipc") || name.startsWith("ozone"))
        return ToolGroupId::Izotope;

    if (name.startsWith("generate_dataset"))
        return ToolGroupId::Dataset;

    if (name.containsIgnoreCase("parameter") || name.containsIgnoreCase("learn")
        || name.containsIgnoreCase("token"))
    {
        return ToolGroupId::ParameterAi;
    }

    return ToolGroupId::Other;
}

void addIfPresent(const juce::StringArray& available, juce::StringArray& examples, const char* name)
{
    const juce::String toolName(name);
    if (available.contains(toolName, false) && !examples.contains(toolName, false))
        examples.add(toolName);
}

juce::String trimNumericLabel(juce::String label)
{
    while (label.endsWithChar('0'))
        label = label.dropLastCharacters(1);
    if (label.endsWithChar('.'))
        label = label.dropLastCharacters(1);
    return label;
}

juce::String formatPercent(double normalized)
{
    return trimNumericLabel(juce::String(normalized * 100.0, 1)) + "%";
}

juce::String formatDiffValue(const nlohmann::json& value, const juce::String& control)
{
    if (value.is_string())
        return juce::String(value.get<std::string>());

    if (value.is_number())
    {
        const auto number = value.get<double>();
        if (control == "source")
        {
            if (std::abs(number) < 0.0001)
                return "xy";
            if (std::abs(number - 1.0) < 0.0001)
                return "fader";
        }

        if (control.contains("fader") || control.contains("morph"))
            return formatPercent(number);

        return trimNumericLabel(juce::String(number, 3));
    }

    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";

    if (value.is_null())
        return "-";

    return juce::String(value.dump());
}

juce::String buildPreviewDiffLine(const nlohmann::json& preview)
{
    if (!preview.is_object() || !preview.contains("diffs") || !preview["diffs"].is_array())
        return {};

    juce::StringArray diffs;
    for (const auto& diff : preview["diffs"])
    {
        if (!diff.is_object())
            continue;

        const auto control = juce::String(diff.value("control",
            diff.value("name", diff.value("parameter_id", std::string{}))));
        if (control.isEmpty())
            continue;

        const auto before = diff.contains("before") ? diff["before"] : nlohmann::json();
        const auto after = diff.contains("after") ? diff["after"] : nlohmann::json();
        diffs.add(control + " " + formatDiffValue(before, control)
            + " -> " + formatDiffValue(after, control));

        if (diffs.size() >= 4)
            break;
    }

    if (diffs.isEmpty())
        return {};

    auto line = "Plan Preview: " + diffs.joinIntoString("; ");
    const auto remaining = static_cast<int>(preview["diffs"].size()) - diffs.size();
    if (remaining > 0)
        line << "; +" << remaining << " more";
    return line;
}

juce::String findWorkflowStepToolSummary(const nlohmann::json& workflow)
{
    if (!workflow.is_object() || !workflow.contains("steps") || !workflow["steps"].is_array()
        || workflow["steps"].empty())
        return {};

    const auto& step = workflow["steps"].front();
    if (!step.is_object())
        return {};

    auto summary = juce::String(step.value("toolName", std::string{}));
    const auto state = juce::String(step.value("state", std::string{}));
    if (state.isNotEmpty())
        summary << " (" << state << ")";
    return summary;
}

juce::String buildFeedbackLine(const nlohmann::json& response)
{
    if (!response.is_object() || !response.contains("outcome") || !response["outcome"].is_object())
        return {};

    const auto& outcome = response["outcome"];
    auto line = "Feedback: " + juce::String(outcome.value("feedbackStatus", std::string{"unreviewed"}));
    const auto feedbackText = juce::String(outcome.value("userFeedback", std::string{})).trim();
    if (feedbackText.isNotEmpty())
        line << " - " << feedbackText;
    return line;
}

nlohmann::json approvalArrayFromResponse(const nlohmann::json& approvalResponse)
{
    if (approvalResponse.is_array())
        return approvalResponse;
    if (approvalResponse.is_object() && approvalResponse.contains("approvals")
        && approvalResponse["approvals"].is_array())
        return approvalResponse["approvals"];
    return nlohmann::json::array();
}

nlohmann::json firstPendingApproval(const nlohmann::json& approvalResponse)
{
    const auto approvals = approvalArrayFromResponse(approvalResponse);
    for (const auto& approval : approvals)
    {
        if (approval.is_object()
            && juce::String(approval.value("status", std::string{})).equalsIgnoreCase("pending"))
            return approval;
    }

    return nlohmann::json::object();
}

} // namespace

AIChatPanel::AIChatPanel(MorePhiProcessor& processor)
    : processor_(processor)
    , llmSettings_(LLMSettings::createDefault())
    , localAssistant_(processor)
    , llmChatClient_(processor)
{
    providerLabel_.setJustificationType(juce::Justification::centredLeft);
    statusChip_.setJustificationType(juce::Justification::centred);
    approvalsButton_.onClick = [this]() { refreshApprovalPanel(true); };
    settingsButton_.onClick = [this]() { showLLMSettingsDialog(); };

    prompt_.setMultiLine(false);
    prompt_.setReturnKeyStartsNewLine(false);
    prompt_.setTextToShowWhenEmpty("Ask the assistant", juce::Colour(0xff8a93a3));
    prompt_.onReturnKey = [this]() { submitPrompt(); };

    sendButton_.onClick   = [this]() { submitPrompt(); };
    cancelButton_.onClick = [this]() { cancelChat(); };
    cancelButton_.setEnabled(false);
    clearButton_.onClick = [this]()
    {
        transcript_.clearMessages();
        conversationHistory_.clear();
        lastWorkflowResult_ = {};
        setWorkflowPanelVisible(false);
        currentApprovalId_.clear();
        lastApprovalQueue_ = nlohmann::json::object();
        setApprovalPanelVisible(false);
    };

    workflowStatusLabel_.setJustificationType(juce::Justification::centredLeft);
    workflowStatusLabel_.setMinimumHorizontalScale(0.72f);
    workflowDetailLabel_.setJustificationType(juce::Justification::centredLeft);
    workflowDetailLabel_.setMinimumHorizontalScale(0.68f);

    workflowAcceptedButton_.setTooltip("Mark the last assistant workflow as accepted.");
    workflowRejectedButton_.setTooltip("Mark the last assistant workflow as rejected.");
    workflowBetterButton_.setTooltip("Mark the last assistant workflow as sounding better.");
    workflowTooMuchButton_.setTooltip("Mark the last assistant workflow as too much.");
    workflowUndoButton_.setTooltip("Roll back the last assistant workflow.");
    workflowAcceptedButton_.onClick = [this]() { handleWorkflowFeedbackButton("that was accepted"); };
    workflowRejectedButton_.onClick = [this]() { handleWorkflowFeedbackButton("that was rejected"); };
    workflowBetterButton_.onClick = [this]() { handleWorkflowFeedbackButton("that sounded better"); };
    workflowTooMuchButton_.onClick = [this]() { handleWorkflowFeedbackButton("that was too much"); };
    workflowUndoButton_.onClick = [this]() { handleWorkflowUndoButton(); };

    approvalStatusLabel_.setJustificationType(juce::Justification::centredLeft);
    approvalStatusLabel_.setMinimumHorizontalScale(0.72f);
    approvalDetailLabel_.setJustificationType(juce::Justification::centredLeft);
    approvalDetailLabel_.setMinimumHorizontalScale(0.68f);
    approvalApproveButton_.setTooltip("Approve the selected pending MCP action.");
    approvalRejectButton_.setTooltip("Reject the selected pending MCP action.");
    approvalRefreshButton_.setTooltip("Refresh pending permission approval requests.");
    approvalApproveButton_.onClick = [this]() { handleApprovalDecision(true); };
    approvalRejectButton_.onClick = [this]() { handleApprovalDecision(false); };
    approvalRefreshButton_.onClick = [this]() { refreshApprovalPanel(true); };

    addAndMakeVisible(providerLabel_);
    addAndMakeVisible(statusChip_);
    addAndMakeVisible(approvalsButton_);
    addAndMakeVisible(settingsButton_);
    addAndMakeVisible(transcript_);
    addAndMakeVisible(approvalPanel_);
    addAndMakeVisible(approvalStatusLabel_);
    addAndMakeVisible(approvalDetailLabel_);
    addAndMakeVisible(approvalApproveButton_);
    addAndMakeVisible(approvalRejectButton_);
    addAndMakeVisible(approvalRefreshButton_);
    addAndMakeVisible(workflowPanel_);
    addAndMakeVisible(workflowStatusLabel_);
    addAndMakeVisible(workflowDetailLabel_);
    addAndMakeVisible(workflowAcceptedButton_);
    addAndMakeVisible(workflowRejectedButton_);
    addAndMakeVisible(workflowBetterButton_);
    addAndMakeVisible(workflowTooMuchButton_);
    addAndMakeVisible(workflowUndoButton_);
    addAndMakeVisible(prompt_);
    addAndMakeVisible(sendButton_);
    addAndMakeVisible(cancelButton_);
    addAndMakeVisible(clearButton_);

    setWorkflowPanelVisible(false);
    setApprovalPanelVisible(false);

    loadLLMSettings();
    refreshLLMToolbar();
}

void AIChatPanel::loadLLMSettings()
{
    juce::String error;
    if (!llmSettingsStore_.load(llmSettings_, error))
    {
        llmSettings_ = LLMSettings::createDefault();
        statusChip_.setText("Failed", juce::dontSendNotification);
        providerLabel_.setText("Provider: None", juce::dontSendNotification);
        transcript_.addMessage(ChatDisplay::Role::System, error);
    }
}

void AIChatPanel::refreshLLMToolbar()
{
    const auto providerName = llmSettings_.getActiveProviderDisplayName();
    providerLabel_.setText("Provider: " + (providerName.isEmpty() ? "None" : providerName),
                           juce::dontSendNotification);
    statusChip_.setText(toDisplayString(llmSettings_.getToolbarStatus()), juce::dontSendNotification);
}

void AIChatPanel::showLLMSettingsDialog()
{
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "LLM Settings";
    options.dialogBackgroundColour = juce::Colour(0xff121826);
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = false;

    options.content.setOwned(new LLMSettingsDialog(
        llmSettings_,
        llmSettingsStore_,
        llmValidator_,
        [this](const LLMSettings& savedSettings)
        {
            llmSettings_ = savedSettings;
            refreshLLMToolbar();
        }));

    options.launchAsync();
}

void AIChatPanel::trimConversationHistory()
{
    if (conversationHistory_.isEmpty())
        return;

    try
    {
        auto messages = nlohmann::json::parse(conversationHistory_.toStdString());
        if (!messages.is_array() || messages.size() <= 30)
            return;

        nlohmann::json trimmed = nlohmann::json::array();

        if (!messages.empty() && messages[0].value("role", "") == "system")
            trimmed.push_back(messages[0]);

        const auto keepFrom = messages.size() - 28;
        for (std::size_t i = (keepFrom > 1 ? keepFrom : 1); i < messages.size(); ++i)
            trimmed.push_back(messages[i]);

        conversationHistory_ = juce::String(trimmed.dump());
    }
    catch (...) {}
}

void AIChatPanel::submitPrompt()
{
    if (chatPending_)
        return;

    auto text = prompt_.getText().trim();
    if (text.isEmpty())
        return;

    prompt_.clear();
    transcript_.addMessage(ChatDisplay::Role::User, text);

    if (tryHandleLocalWorkflowPrompt(text))
        return;

    if (tryHandleLocalDiagnosticPrompt(text))
        return;

    transcript_.addMessage(ChatDisplay::Role::Assistant, "Thinking...");

    chatPending_ = true;
    chatStartMs_ = juce::Time::currentTimeMillis();
    chatCancelled_ = std::make_shared<std::atomic<bool>>(false);
    sendButton_.setEnabled(false);
    cancelButton_.setEnabled(true);

    auto cancelled = chatCancelled_;
    trimConversationHistory();
    llmChatClient_.chat(llmSettings_, conversationHistory_, text,
        [this, cancelled](juce::String replyText, juce::String errorMsg, juce::String updatedHistory)
        {
            if (*cancelled) return;
            onChatReply(std::move(replyText), std::move(errorMsg), std::move(updatedHistory));
        },
        [this, cancelled](int /*iteration*/, int /*maxIter*/, juce::String status)
        {
            if (*cancelled) return;
            transcript_.updateLastMessage("Thinking... " + status
                + " (" + juce::String((juce::Time::currentTimeMillis() - chatStartMs_) / 1000) + "s)");
        });

    scheduleThinkingUpdate();
}

bool AIChatPanel::detectsLocalMcpToolInventoryPromptForTest(const juce::String& text)
{
    return isLocalMcpToolInventoryPrompt(text);
}

bool AIChatPanel::detectsLocalWorkflowPromptForTest(const juce::String& text)
{
    return AIAssistant::detectsLocalWorkflowPrompt(text);
}

bool AIChatPanel::detectsLocalWorkflowFeedbackPromptForTest(const juce::String& text)
{
    return AIAssistant::detectsLocalFeedbackPrompt(text);
}

bool AIChatPanel::detectsLocalUndoPromptForTest(const juce::String& text)
{
    return AIAssistant::detectsLocalUndoPrompt(text);
}

juce::String AIChatPanel::buildLocalMcpToolInventoryReplyForTest()
{
    return buildLocalMcpToolInventoryReply();
}

juce::String AIChatPanel::buildWorkflowTimelineTextForTest(const AssistantWorkflowResult& result)
{
    return buildWorkflowTimelineText(result);
}

juce::String AIChatPanel::buildApprovalQueueTextForTest(const nlohmann::json& approvalResponse)
{
    return buildApprovalQueueText(approvalResponse);
}

juce::String AIChatPanel::formatChatErrorForTest(const juce::String& error)
{
    return formatChatErrorForDisplay(error);
}

bool AIChatPanel::isLocalMcpToolInventoryPrompt(const juce::String& text)
{
    const auto lower = text.toLowerCase();
    const bool asksAboutTools = lower.contains("tool") || lower.contains("tools");
    const bool mentionsMcp = lower.contains("mcp");
    const bool asksInventory = lower.contains("what") || lower.contains("which")
        || lower.contains("list") || lower.contains("available")
        || lower.contains("access") || lower.contains("have access");

    return asksAboutTools && asksInventory
        && (mentionsMcp || lower.contains("what tools do you have access"));
}

juce::String AIChatPanel::buildLocalMcpToolInventoryReply()
{
    try
    {
        const auto parsed = nlohmann::json::parse(MCPToolHandler::getToolList().toStdString());
        const auto tools = parsed.contains("tools") && parsed["tools"].is_array()
            ? parsed["tools"]
            : nlohmann::json::array();

        auto groups = makeEmptyToolGroups();
        juce::StringArray toolNames;

        for (const auto& tool : tools)
        {
            const auto name = juce::String(tool.value("name", std::string{}));
            if (name.isEmpty())
                continue;

            toolNames.add(name);
            ++groups[static_cast<std::size_t>(classifyToolName(name))].count;
        }

        toolNames.sort(true);

        juce::StringArray examples;
        addIfPresent(toolNames, examples, "more_phi.parameters");
        addIfPresent(toolNames, examples, "more_phi.set_parameter");
        addIfPresent(toolNames, examples, "hosted_plugin.parameters");
        addIfPresent(toolNames, examples, "hosted_plugin.set_parameter");
        addIfPresent(toolNames, examples, "capture_snapshot");
        addIfPresent(toolNames, examples, "recall_snapshot");
        addIfPresent(toolNames, examples, "set_morph_position");
        addIfPresent(toolNames, examples, "run_self_test");
        addIfPresent(toolNames, examples, "mastering.plan_preview");
        addIfPresent(toolNames, examples, "plugin_profile.describe_semantic_map");
        addIfPresent(toolNames, examples, "generate_dataset_v3");

        for (const auto& name : toolNames)
        {
            if (examples.size() >= 12)
                break;
            if (!examples.contains(name, false))
                examples.add(name);
        }

        juce::String reply;
        reply << "I can access " << toolNames.size()
              << " local More-Phi MCP tools through the in-plugin tool registry.\n\n"
              << "Main groups:";

        for (const auto& group : groups)
        {
            if (group.count > 0)
                reply << "\n- " << group.label << ": " << group.count;
        }

        if (!examples.isEmpty())
            reply << "\n\nUseful examples: " << examples.joinIntoString(", ");

        reply << "\n\nThese local MCP tools are separate from the remote LLM provider; "
              << "a WinHTTP/NVIDIA timeout does not mean the local MCP server is unavailable.";
        return reply;
    }
    catch (const std::exception& e)
    {
        return "I can query the local MCP tool registry, but parsing the tool list failed: "
             + juce::String(e.what());
    }
    catch (...)
    {
        return "I can query the local MCP tool registry, but parsing the tool list failed.";
    }
}

juce::String AIChatPanel::buildWorkflowTimelineText(const AssistantWorkflowResult& result)
{
    juce::StringArray lines;

    const auto workflow = result.response.value("workflow_run", nlohmann::json::object());
    const auto responseState = workflow.is_object()
        ? juce::String(workflow.value("state", std::string{}))
        : juce::String();
    const auto state = responseState.isNotEmpty()
        ? responseState
        : (result.success ? juce::String("completed") : juce::String("failed"));

    if (result.workflowRunId.isNotEmpty())
        lines.add("WorkflowRun " + result.workflowRunId + " " + state);
    else if (result.handled)
        lines.add("Workflow " + state);
    else
        lines.add("No local workflow");

    if (result.transactionId.isNotEmpty())
        lines.add("Transaction: " + result.transactionId);
    if (result.approvalId.isNotEmpty())
        lines.add("Approval: " + result.approvalId);

    const auto stepSummary = findWorkflowStepToolSummary(workflow);
    if (stepSummary.isNotEmpty())
        lines.add("Step: " + stepSummary);

    const auto previewLine = buildPreviewDiffLine(result.preview);
    if (previewLine.isNotEmpty())
        lines.add(previewLine);

    if (workflow.is_object() && workflow.contains("finalReport") && workflow["finalReport"].is_object())
    {
        const auto verification = juce::String(workflow["finalReport"].value("verification", std::string{}));
        if (verification.isNotEmpty())
            lines.add("Verify: " + verification);
    }

    const auto feedbackLine = buildFeedbackLine(result.response);
    if (feedbackLine.isNotEmpty())
        lines.add(feedbackLine);

    if (!result.success && result.message.isNotEmpty())
        lines.add("Error: " + result.message);

    return lines.joinIntoString("\n");
}

juce::String AIChatPanel::buildApprovalQueueText(const nlohmann::json& approvalResponse)
{
    const auto approval = firstPendingApproval(approvalResponse);
    if (!approval.is_object() || approval.empty())
        return "No pending approval requests.";

    juce::StringArray lines;
    const auto id = juce::String(approval.value("id", std::string{}));
    const auto toolName = juce::String(approval.value("toolName", std::string{}));
    const auto risk = juce::String(approval.value("risk", std::string{}));
    const auto workflowRunId = juce::String(approval.value("workflowRunId", std::string{}));

    lines.add("Approval Required: " + risk + " " + toolName);
    if (id.isNotEmpty())
        lines.add("Approval ID: " + id);
    if (workflowRunId.isNotEmpty())
        lines.add("WorkflowRun: " + workflowRunId);

    if (approval.contains("predictedDiff") && approval["predictedDiff"].is_object())
    {
        const auto previewLine = buildPreviewDiffLine(approval["predictedDiff"]);
        if (previewLine.isNotEmpty())
            lines.add(previewLine);
    }

    const auto explanation = juce::String(approval.value("explanation", std::string{})).trim();
    if (explanation.isNotEmpty())
        lines.add("Reason: " + explanation);

    return lines.joinIntoString("\n");
}

juce::String AIChatPanel::formatChatErrorForDisplay(const juce::String& error)
{
    const auto lower = error.toLowerCase();

    if (lower.contains("winhttp") && lower.contains("12002"))
    {
        if (lower.contains("transport layer") || lower.contains("nvidia chat request"))
            return error;

        return "NVIDIA chat request timed out before a response arrived. "
               "The local MCP server can still be running; this timeout is from the remote "
               "provider/network path. Try Fetch Models, choose a tool-capable NVIDIA model, "
               "or retry after the model warms up. Raw transport detail: " + error;
    }

    if (lower.contains("winhttp"))
    {
        if (lower.contains("transport layer"))
            return error;

        return "Chat request failed at the Windows HTTP transport layer. "
               "Check the provider URL, API key, network/proxy settings, and selected model. "
               "Raw transport detail: " + error;
    }

    return error;
}

bool AIChatPanel::tryHandleLocalWorkflowPrompt(const juce::String& text)
{
    const bool feedbackPrompt = AIAssistant::detectsLocalFeedbackPrompt(text);
    if (!feedbackPrompt && !AIAssistant::detectsLocalWorkflowPrompt(text))
        return false;

    transcript_.addMessage(ChatDisplay::Role::Assistant, "Planning local workflow...");

    AssistantWorkflowResult result;
    if (AIAssistant::detectsLocalUndoPrompt(text))
        result = localAssistant_.undoLastAssistantWorkflow();
    else if (feedbackPrompt)
        result = localAssistant_.recordFeedbackForLastWorkflow(text);
    else
        result = localAssistant_.executeLocalWorkflowPrompt(text);

    if (!result.handled)
        return false;

    transcript_.updateLastMessage(result.message.isEmpty()
        ? juce::String("Assistant workflow did not produce a response.")
        : result.message);
    updateWorkflowPanel(result);
    return true;
}

void AIChatPanel::handleWorkflowFeedbackButton(const juce::String& feedbackText)
{
    if (!workflowPanel_.isVisible() || lastWorkflowResult_.transactionId.isEmpty())
        return;

    transcript_.addMessage(ChatDisplay::Role::User, feedbackText);
    transcript_.addMessage(ChatDisplay::Role::Assistant, "Recording workflow feedback...");

    auto result = localAssistant_.recordFeedbackForLastWorkflow(feedbackText);
    transcript_.updateLastMessage(result.message.isEmpty()
        ? juce::String("Assistant workflow did not produce a feedback response.")
        : result.message);
    updateWorkflowPanel(result);
}

void AIChatPanel::handleWorkflowUndoButton()
{
    if (!workflowPanel_.isVisible() || lastWorkflowResult_.transactionId.isEmpty())
        return;

    transcript_.addMessage(ChatDisplay::Role::User, "undo last assistant workflow");
    transcript_.addMessage(ChatDisplay::Role::Assistant, "Rolling back workflow...");

    auto result = localAssistant_.undoLastAssistantWorkflow();
    transcript_.updateLastMessage(result.message.isEmpty()
        ? juce::String("Assistant workflow did not produce an undo response.")
        : result.message);
    updateWorkflowPanel(result);
}

void AIChatPanel::updateWorkflowPanel(const AssistantWorkflowResult& result)
{
    if (!result.handled)
        return;

    lastWorkflowResult_ = result;
    const auto timeline = buildWorkflowTimelineText(result);
    juce::StringArray lines;
    lines.addLines(timeline);

    workflowStatusLabel_.setText(lines.isEmpty() ? juce::String("Workflow") : lines[0],
                                 juce::dontSendNotification);

    if (!lines.isEmpty())
        lines.remove(0);
    workflowDetailLabel_.setText(lines.joinIntoString(" | "), juce::dontSendNotification);

    const bool rolledBack = result.message.containsIgnoreCase("rolled back");
    const bool canAct = result.success && result.transactionId.isNotEmpty() && !rolledBack;
    workflowAcceptedButton_.setEnabled(canAct);
    workflowRejectedButton_.setEnabled(canAct);
    workflowBetterButton_.setEnabled(canAct);
    workflowTooMuchButton_.setEnabled(canAct);
    workflowUndoButton_.setEnabled(canAct);

    setWorkflowPanelVisible(true);
    if (result.awaitingApproval)
        refreshApprovalPanel(false);
}

void AIChatPanel::setWorkflowPanelVisible(bool visible)
{
    workflowPanel_.setVisible(visible);
    workflowStatusLabel_.setVisible(visible);
    workflowDetailLabel_.setVisible(visible);
    workflowAcceptedButton_.setVisible(visible);
    workflowRejectedButton_.setVisible(visible);
    workflowBetterButton_.setVisible(visible);
    workflowTooMuchButton_.setVisible(visible);
    workflowUndoButton_.setVisible(visible);
    resized();
}

void AIChatPanel::refreshApprovalPanel(bool announceWhenEmpty)
{
    try
    {
        const auto raw = MCPToolHandler::handle("permission.list_approvals",
                                                {},
                                                processor_,
                                                processor_.getInstanceIdentity());
        const auto parsed = nlohmann::json::parse(raw.toStdString());
        updateApprovalPanelFromQueue(parsed);

        if (announceWhenEmpty && currentApprovalId_.isEmpty())
            transcript_.addMessage(ChatDisplay::Role::System, buildApprovalQueueText(parsed));
    }
    catch (const std::exception& e)
    {
        setApprovalPanelVisible(false);
        if (announceWhenEmpty)
            transcript_.addMessage(ChatDisplay::Role::System,
                "Could not refresh approval queue: " + juce::String(e.what()));
    }
    catch (...)
    {
        setApprovalPanelVisible(false);
        if (announceWhenEmpty)
            transcript_.addMessage(ChatDisplay::Role::System, "Could not refresh approval queue.");
    }
}

void AIChatPanel::handleApprovalDecision(bool approve)
{
    if (currentApprovalId_.isEmpty())
        return;

    const auto method = approve ? juce::String("permission.approve") : juce::String("permission.reject");
    transcript_.addMessage(ChatDisplay::Role::User,
        (approve ? juce::String("approve ") : juce::String("reject ")) + currentApprovalId_);
    transcript_.addMessage(ChatDisplay::Role::Assistant,
        approve ? juce::String("Approving pending action...") : juce::String("Rejecting pending action..."));

    try
    {
        const auto raw = MCPToolHandler::handle(method,
            juce::JSON::parse(juce::String(nlohmann::json{{"approval_id", currentApprovalId_.toStdString()}}.dump())),
            processor_,
            processor_.getInstanceIdentity());
        const auto parsed = nlohmann::json::parse(raw.toStdString());
        const bool success = parsed.value("success", false);
        transcript_.updateLastMessage(success
            ? (approve ? juce::String("Approval accepted.") : juce::String("Approval rejected."))
            : ("Approval decision failed: " + juce::String(parsed.value("error", "permission_decision_failed"))));
    }
    catch (const std::exception& e)
    {
        transcript_.updateLastMessage("Approval decision failed: " + juce::String(e.what()));
    }
    catch (...)
    {
        transcript_.updateLastMessage("Approval decision failed.");
    }

    refreshApprovalPanel(false);
}

void AIChatPanel::updateApprovalPanelFromQueue(const nlohmann::json& approvalResponse)
{
    lastApprovalQueue_ = approvalResponse;
    const auto approval = firstPendingApproval(approvalResponse);
    if (!approval.is_object() || approval.empty())
    {
        currentApprovalId_.clear();
        setApprovalPanelVisible(false);
        return;
    }

    currentApprovalId_ = juce::String(approval.value("id", std::string{}));
    const auto text = buildApprovalQueueText(approvalResponse);
    juce::StringArray lines;
    lines.addLines(text);

    approvalStatusLabel_.setText(lines.isEmpty() ? juce::String("Approval Required") : lines[0],
                                 juce::dontSendNotification);
    if (!lines.isEmpty())
        lines.remove(0);
    approvalDetailLabel_.setText(lines.joinIntoString(" | "), juce::dontSendNotification);

    const bool canDecide = currentApprovalId_.isNotEmpty();
    approvalApproveButton_.setEnabled(canDecide);
    approvalRejectButton_.setEnabled(canDecide);
    approvalRefreshButton_.setEnabled(true);
    setApprovalPanelVisible(true);
}

void AIChatPanel::setApprovalPanelVisible(bool visible)
{
    approvalPanel_.setVisible(visible);
    approvalStatusLabel_.setVisible(visible);
    approvalDetailLabel_.setVisible(visible);
    approvalApproveButton_.setVisible(visible);
    approvalRejectButton_.setVisible(visible);
    approvalRefreshButton_.setVisible(visible);
    resized();
}

bool AIChatPanel::tryHandleLocalDiagnosticPrompt(const juce::String& text)
{
    if (isLocalMcpToolInventoryPrompt(text))
    {
        transcript_.addMessage(ChatDisplay::Role::Assistant, buildLocalMcpToolInventoryReply());
        return true;
    }

    const auto lower = text.toLowerCase();
    const bool asksForDiagnostic = lower.contains("diagnostic")
        || lower.contains("self test")
        || lower.contains("self-test")
        || lower.contains("snapshot suite");

    if (!asksForDiagnostic)
        return false;

    transcript_.addMessage(ChatDisplay::Role::Assistant, "Running local diagnostic...");

    auto* object = new juce::DynamicObject();
    object->setProperty("suite", lower.contains("full") ? "full" : "snapshot");
    const juce::var params(object);

    auto result = MCPToolHandler::handle("run_self_test",
                                         params,
                                         processor_,
                                         processor_.getInstanceIdentity());
    transcript_.updateLastMessage("Local diagnostic report:\n" + result);
    return true;
}

void AIChatPanel::cancelChat()
{
    if (!chatPending_) return;
    *chatCancelled_ = true;
    chatPending_ = false;
    sendButton_.setEnabled(true);
    cancelButton_.setEnabled(false);
    transcript_.updateLastMessage("(cancelled)");
}

void AIChatPanel::scheduleThinkingUpdate()
{
    if (!chatPending_) return;
    const int secs = static_cast<int>((juce::Time::currentTimeMillis() - chatStartMs_) / 1000);
    transcript_.updateLastMessage("Thinking... (" + juce::String(secs) + "s)");

    juce::Component::SafePointer<AIChatPanel> safeThis(this);
    juce::Timer::callAfterDelay(2000, [safeThis]()
    {
        if (safeThis != nullptr)
            safeThis->scheduleThinkingUpdate();
    });
}

void AIChatPanel::onChatReply(juce::String text, juce::String error, juce::String updatedHistory)
{
    // Called on message thread
    chatPending_ = false;
    sendButton_.setEnabled(true);
    cancelButton_.setEnabled(false);

    if (!error.isEmpty())
    {
        transcript_.updateLastMessage("[Error] " + formatChatErrorForDisplay(error));
        return;
    }

    conversationHistory_ = updatedHistory;
    transcript_.updateLastMessage(text.isEmpty() ? "(empty response)" : text);
}

void AIChatPanel::resized()
{
    auto area = getLocalBounds().reduced(8);

    // ── Toolbar row ──────────────────────────────────────────────────────────
    auto toolbar = area.removeFromTop(28);
    settingsButton_.setBounds(toolbar.removeFromRight(110).reduced(2, 1));
    approvalsButton_.setBounds(toolbar.removeFromRight(88).reduced(2, 1));
    statusChip_.setBounds(toolbar.removeFromRight(90).reduced(4, 3));
    providerLabel_.setBounds(toolbar.reduced(0, 1));

    area.removeFromTop(4);

    // ── Input row ────────────────────────────────────────────────────────────
    auto inputRow = area.removeFromBottom(34);
    inputRow.removeFromTop(4);
    clearButton_.setBounds(inputRow.removeFromRight(60).reduced(2, 1));
    cancelButton_.setBounds(inputRow.removeFromRight(64).reduced(2, 1));
    sendButton_ .setBounds(inputRow.removeFromRight(60).reduced(2, 1));
    prompt_.setBounds(inputRow.reduced(0, 1));

    if (approvalPanel_.isVisible())
    {
        area.removeFromBottom(6);
        auto approvalRow = area.removeFromBottom(72);
        approvalPanel_.setBounds(approvalRow);

        auto approvalContent = approvalRow.reduced(10, 16);
        auto buttonArea = approvalContent.removeFromRight(230);
        approvalRefreshButton_.setBounds(buttonArea.removeFromRight(76).reduced(3, 5));
        approvalRejectButton_.setBounds(buttonArea.removeFromRight(70).reduced(3, 5));
        approvalApproveButton_.setBounds(buttonArea.removeFromRight(78).reduced(3, 5));

        auto statusRow = approvalContent.removeFromTop(22);
        approvalStatusLabel_.setBounds(statusRow);
        approvalDetailLabel_.setBounds(approvalContent.reduced(0, 2));
    }
    else
    {
        approvalPanel_.setBounds({});
        approvalStatusLabel_.setBounds({});
        approvalDetailLabel_.setBounds({});
        approvalApproveButton_.setBounds({});
        approvalRejectButton_.setBounds({});
        approvalRefreshButton_.setBounds({});
    }

    if (workflowPanel_.isVisible())
    {
        area.removeFromBottom(6);
        auto workflowRow = area.removeFromBottom(72);
        workflowPanel_.setBounds(workflowRow);

        auto workflowContent = workflowRow.reduced(10, 16);
        auto buttonArea = workflowContent.removeFromRight(410);
        workflowUndoButton_.setBounds(buttonArea.removeFromRight(68).reduced(3, 5));
        workflowTooMuchButton_.setBounds(buttonArea.removeFromRight(84).reduced(3, 5));
        workflowBetterButton_.setBounds(buttonArea.removeFromRight(70).reduced(3, 5));
        workflowRejectedButton_.setBounds(buttonArea.removeFromRight(80).reduced(3, 5));
        workflowAcceptedButton_.setBounds(buttonArea.removeFromRight(90).reduced(3, 5));

        auto statusRow = workflowContent.removeFromTop(22);
        workflowStatusLabel_.setBounds(statusRow);
        workflowDetailLabel_.setBounds(workflowContent.reduced(0, 2));
    }
    else
    {
        workflowPanel_.setBounds({});
        workflowStatusLabel_.setBounds({});
        workflowDetailLabel_.setBounds({});
        workflowAcceptedButton_.setBounds({});
        workflowRejectedButton_.setBounds({});
        workflowBetterButton_.setBounds({});
        workflowTooMuchButton_.setBounds({});
        workflowUndoButton_.setBounds({});
    }

    // ── Transcript fills the rest ─────────────────────────────────────────────
    transcript_.setBounds(area);
}

} // namespace more_phi
