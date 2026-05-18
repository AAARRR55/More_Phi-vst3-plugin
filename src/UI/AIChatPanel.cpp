/*
 * More-Phi - UI/AIChatPanel.cpp
 */
#include "AIChatPanel.h"
#include "LLMSettingsDialog.h"
#include "AI/MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"

#include <nlohmann/json.hpp>

#include <array>
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

} // namespace

AIChatPanel::AIChatPanel(MorePhiProcessor& processor)
    : processor_(processor)
    , llmSettings_(LLMSettings::createDefault())
    , llmChatClient_(processor)
{
    providerLabel_.setJustificationType(juce::Justification::centredLeft);
    statusChip_.setJustificationType(juce::Justification::centred);
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
    };

    addAndMakeVisible(providerLabel_);
    addAndMakeVisible(statusChip_);
    addAndMakeVisible(settingsButton_);
    addAndMakeVisible(transcript_);
    addAndMakeVisible(prompt_);
    addAndMakeVisible(sendButton_);
    addAndMakeVisible(cancelButton_);
    addAndMakeVisible(clearButton_);

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
        [this, cancelled](int iteration, int /*maxIter*/, juce::String status)
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

juce::String AIChatPanel::buildLocalMcpToolInventoryReplyForTest()
{
    return buildLocalMcpToolInventoryReply();
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

    // ── Transcript fills the rest ─────────────────────────────────────────────
    transcript_.setBounds(area);
}

} // namespace more_phi
