/*
 * More-Phi — AI/AIAssistant.cpp
 */
#include "AIAssistant.h"
#include "MCPToolHandler.h"
#include "Plugin/PluginProcessor.h"

#include <cmath>
#include <exception>
#include <optional>
#include <regex>
#include <string>

namespace more_phi {

namespace {

struct ParsedMorphCommand
{
    bool handled = false;
    bool valid = false;
    bool useFader = true;
    double fader = 0.5;
    double x = 0.5;
    double y = 0.5;
    juce::String error;
};

bool containsAny(const juce::String& text, std::initializer_list<const char*> needles)
{
    for (const auto* needle : needles)
        if (text.contains(needle))
            return true;
    return false;
}

bool looksLikePositiveFeedback(const juce::String& lower)
{
    return containsAny(lower, {"better", "good", "great", "nice", "like that", "works", "worked", "keep", "improved"});
}

bool looksLikeNegativeFeedback(const juce::String& lower)
{
    return containsAny(lower, {"too much", "worse", "bad", "not good", "don't like", "do not like", "reject", "wrong", "harsh", "muddy", "overdid"});
}

juce::String workflowFeedbackStatusFromText(const juce::String& text)
{
    const auto lower = text.trim().toLowerCase();
    if (containsAny(lower, {"undo", "revert", "rollback", "roll back"}))
        return "undo";
    if (containsAny(lower, {"too much", "overdid", "overdone", "over done"}))
        return "too_much";
    if (containsAny(lower, {"sounded better", "sounds better", "sound better", "better", "improved"}))
        return "sounds_better";
    if (looksLikeNegativeFeedback(lower))
        return "rejected";
    if (looksLikePositiveFeedback(lower))
        return "accepted";

    return "accepted";
}

juce::String percentLabel(double normalized)
{
    auto label = juce::String(normalized * 100.0, 1);
    while (label.endsWithChar('0'))
        label = label.dropLastCharacters(1);
    if (label.endsWithChar('.'))
        label = label.dropLastCharacters(1);
    return label + "%";
}

std::optional<double> parseExplicitNumber(const juce::String& lower, bool& usedPercent)
{
    static const std::regex numberPattern(R"(([-+]?(?:\d+(?:\.\d*)?|\.\d+))\s*(%|percent|per cent)?)",
                                          std::regex::icase);

    const auto text = lower.toStdString();
    std::smatch match;
    if (!std::regex_search(text, match, numberPattern))
        return std::nullopt;

    usedPercent = match.size() > 2 && match[2].matched && !match[2].str().empty();
    return std::stod(match[1].str());
}

std::optional<double> parseAxisNumber(const juce::String& lower, char axis, juce::String& error)
{
    const std::string pattern = std::string(R"(\b)") + axis
        + R"(\s*(?:=|:|to)?\s*([-+]?(?:\d+(?:\.\d*)?|\.\d+))\s*(%|percent|per cent)?)";
    const std::regex axisPattern(pattern, std::regex::icase);

    const auto text = lower.toStdString();
    std::smatch match;
    if (!std::regex_search(text, match, axisPattern))
        return std::nullopt;

    const bool usedPercent = match.size() > 2 && match[2].matched && !match[2].str().empty();
    double value = std::stod(match[1].str());
    if (usedPercent || value > 1.0)
        value /= 100.0;

    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
    {
        error = "Morph values must be between 0% and 100%, or normalized between 0.0 and 1.0.";
        return std::nullopt;
    }

    return value;
}

ParsedMorphCommand parseMorphCommand(const juce::String& text)
{
    ParsedMorphCommand parsed;
    const auto lower = text.trim().toLowerCase();
    if (lower.isEmpty() || !lower.contains("morph"))
        return parsed;

    const bool hasAction = containsAny(lower, {"set", "move", "put", "place", "change"});
    const bool mentionsTarget = containsAny(lower, {"fader", "slider", "pad", "xypad", "xy", "position", "pos"});
    if (!hasAction || !mentionsTarget)
        return parsed;

    parsed.handled = true;
    parsed.useFader = containsAny(lower, {"fader", "slider"})
        && !containsAny(lower, {"pad", "xypad", "xy"});

    if (!parsed.useFader)
    {
        juce::String axisError;
        const auto x = parseAxisNumber(lower, 'x', axisError);
        const auto y = parseAxisNumber(lower, 'y', axisError);
        if (axisError.isNotEmpty())
        {
            parsed.error = axisError;
            return parsed;
        }

        if (x.has_value() && y.has_value())
        {
            parsed.x = *x;
            parsed.y = *y;
            parsed.valid = true;
            return parsed;
        }

        if (containsAny(lower, {"center", "centre", "middle"}))
        {
            parsed.x = 0.5;
            parsed.y = 0.5;
            parsed.valid = true;
            return parsed;
        }

        parsed.error = "Morph pad commands need both X and Y values, for example: move the morph pad to x 25% y 75%.";
        return parsed;
    }

    if (containsAny(lower, {"center", "centre", "middle"}))
    {
        parsed.fader = 0.5;
        parsed.valid = true;
        return parsed;
    }

    if (containsAny(lower, {"minimum", "min", "zero", "start", "left"}))
    {
        parsed.fader = 0.0;
        parsed.valid = true;
        return parsed;
    }

    if (containsAny(lower, {"maximum", "max", "full", "end", "right"}))
    {
        parsed.fader = 1.0;
        parsed.valid = true;
        return parsed;
    }

    bool usedPercent = false;
    const auto maybeNumber = parseExplicitNumber(lower, usedPercent);
    if (!maybeNumber.has_value())
    {
        parsed.error = "I can handle local morph commands when they include a target value, for example: move the morph fader to 42%.";
        return parsed;
    }

    double value = *maybeNumber;
    if (usedPercent || value > 1.0)
        value /= 100.0;

    if (!std::isfinite(value) || value < 0.0 || value > 1.0)
    {
        parsed.error = "Morph values must be between 0% and 100%, or normalized between 0.0 and 1.0.";
        return parsed;
    }

    parsed.fader = value;
    parsed.valid = true;
    return parsed;
}

juce::var jsonToVar(const nlohmann::json& value)
{
    return juce::JSON::parse(juce::String(value.dump()));
}

nlohmann::json parseJsonResponse(const juce::String& response)
{
    try
    {
        return nlohmann::json::parse(response.toStdString());
    }
    catch (const std::exception& e)
    {
        return nlohmann::json{{"success", false}, {"error", "json_parse_failed"}, {"details", e.what()}};
    }
    catch (...)
    {
        return nlohmann::json{{"success", false}, {"error", "json_parse_failed"}};
    }
}

nlohmann::json buildMorphPreview(MorePhiProcessor& processor, const nlohmann::json& stepParams)
{
    const auto rawPreview = MCPToolHandler::handle("automation.diff_preview",
        jsonToVar(nlohmann::json{
            {"tool_name", "set_morph_position"},
            {"params", stepParams}
        }),
        processor,
        processor.getInstanceIdentity());
    return parseJsonResponse(rawPreview);
}

nlohmann::json buildAssistantWorkflowContext(MorePhiProcessor& processor)
{
    nlohmann::json context{
        {"source", "in_plugin_assistant"},
        {"planner", "local_natural_language_v1"},
        {"execution_path", "WorkflowRun -> PermissionPolicy -> AutomationTransaction"}
    };

    const auto rawMemory = MCPToolHandler::handle("memory.get_intent_context",
        jsonToVar(nlohmann::json{{"limit", 6}}),
        processor,
        processor.getInstanceIdentity());
    const auto memoryResponse = parseJsonResponse(rawMemory);
    if (memoryResponse.value("success", false) && memoryResponse.contains("intent_context"))
    {
        auto memory = memoryResponse["intent_context"];
        memory["intent_model"] = "memory_intent_context_v1";
        context["memory"] = memory;
    }

    return context;
}

juce::String findRollbackTransactionId(const nlohmann::json& response)
{
    const auto workflow = response.value("workflow_run", nlohmann::json::object());
    const auto observations = workflow.value("observations", nlohmann::json::object());
    const auto steps = observations.value("steps", nlohmann::json::array());
    for (auto it = steps.rbegin(); it != steps.rend(); ++it)
    {
        const auto result = it->value("result", nlohmann::json::object());
        const auto transactionId = result.value("transaction_id", std::string{});
        if (!transactionId.empty())
            return juce::String(transactionId);
    }

    return {};
}

} // namespace

AIAssistant::AIAssistant(MorePhiProcessor& processor)
    : processor_(processor)
{
}

AIAssistant::~AIAssistant() = default;

void AIAssistant::stagePendingChanges(std::vector<ParamChange> changes)
{
    pendingChanges_ = std::move(changes);
    previewActive_ = false;
}

void AIAssistant::clearPendingChanges()
{
    pendingChanges_.clear();
    previewActive_ = false;
}

bool AIAssistant::applyPreview(juce::String* errorMessage)
{
    if (pendingChanges_.empty())
    {
        if (errorMessage != nullptr)
            *errorMessage = "no pending changes";
        return false;
    }

    for (const auto& change : pendingChanges_)
    {
        const int index = canonicalIndex(change);
        if (index < 0)
        {
            if (errorMessage != nullptr)
                *errorMessage = "invalid parameter index";
            return false;
        }

        if (!processor_.enqueueParameterSet(index,
                                            juce::jlimit(0.0f, 1.0f, change.newValue),
                                            MorePhiProcessor::ParameterEditSource::Assistant,
                                            true))
        {
            if (errorMessage != nullptr)
                *errorMessage = "parameter command queue is full";
            return false;
        }
    }

    previewActive_ = true;
    return true;
}

void AIAssistant::rejectPreview()
{
    clearPendingChanges();
}

void AIAssistant::commitPreview()
{
    clearPendingChanges();
}

bool AIAssistant::detectsLocalWorkflowPrompt(const juce::String& text)
{
    return detectsLocalUndoPrompt(text)
        || detectsLocalFeedbackPrompt(text)
        || parseMorphCommand(text).handled;
}

bool AIAssistant::detectsLocalFeedbackPrompt(const juce::String& text)
{
    const auto lower = text.trim().toLowerCase();
    if (lower.isEmpty() || detectsLocalUndoPrompt(text))
        return false;

    const bool hasFeedbackPolarity = looksLikePositiveFeedback(lower) || looksLikeNegativeFeedback(lower);
    if (!hasFeedbackPolarity)
        return false;

    return containsAny(lower, {"that", "result", "change", "move", "workflow", "sound", "sounded", "mix", "assistant", "edit"});
}

bool AIAssistant::detectsLocalUndoPrompt(const juce::String& text)
{
    const auto lower = text.trim().toLowerCase();
    if (lower.isEmpty())
        return false;

    return containsAny(lower, {"undo", "revert", "rollback", "roll back"})
        && containsAny(lower, {"assistant", "workflow", "action", "previous", "last"});
}

AssistantWorkflowPlan AIAssistant::planLocalWorkflowPrompt(const juce::String& text) const
{
    AssistantWorkflowPlan plan;
    const auto parsed = parseMorphCommand(text);
    plan.handled = parsed.handled;
    plan.valid = parsed.valid;
    plan.error = parsed.error;

    if (!plan.handled)
        return plan;

    if (!plan.valid)
    {
        plan.summary = plan.error;
        return plan;
    }

    nlohmann::json stepParams = nlohmann::json::object();
    juce::String targetLabel;
    if (parsed.useFader)
    {
        stepParams = nlohmann::json{{"fader", parsed.fader}, {"source", "fader"}};
        targetLabel = "morph fader to " + percentLabel(parsed.fader);
    }
    else
    {
        stepParams = nlohmann::json{{"x", parsed.x}, {"y", parsed.y}, {"source", "xy"}};
        targetLabel = "morph pad to X " + percentLabel(parsed.x)
            + " / Y " + percentLabel(parsed.y);
    }

    plan.summary = "Plan: set " + targetLabel + " through a WorkflowRun.";
    plan.preview = buildMorphPreview(processor_, stepParams);
    plan.workflowSubmitParams = nlohmann::json{
        {"user_intent", text.toStdString()},
        {"context", buildAssistantWorkflowContext(processor_)},
        {"steps", nlohmann::json::array({
            {
                {"id", "set_morph_position"},
                {"toolName", "set_morph_position"},
                {"params", stepParams},
                {"expectedObservation", stepParams},
                {"rollbackPlan", {{"strategy", "automation.rollback"}}},
                {"riskClass", "low_write"},
                {"maxRetries", 0}
            }
        })}
    };
    return plan;
}

AssistantWorkflowResult AIAssistant::executeLocalWorkflowPrompt(const juce::String& text)
{
    AssistantWorkflowResult result;
    const auto plan = planLocalWorkflowPrompt(text);
    result.handled = plan.handled;
    result.preview = plan.preview;

    if (!plan.handled)
        return result;

    if (!plan.valid)
    {
        result.message = plan.error;
        return result;
    }

    const auto& identity = processor_.getInstanceIdentity();
    const auto submitResponse = MCPToolHandler::handle("workflow.submit",
                                                       jsonToVar(plan.workflowSubmitParams),
                                                       processor_,
                                                       identity);
    auto submitted = parseJsonResponse(submitResponse);
    if (!submitted.value("success", false))
    {
        result.rawResponse = submitResponse;
        result.response = submitted;
        result.message = "Could not submit assistant WorkflowRun: "
            + juce::String(submitted.value("error", "workflow_submit_failed"));
        return result;
    }

    result.workflowRunId = juce::String(
        submitted.value("workflow_run", nlohmann::json::object()).value("id", std::string{}));
    if (result.workflowRunId.isEmpty())
    {
        result.message = "Could not submit assistant WorkflowRun: missing workflow_run.id.";
        return result;
    }

    const auto executeResponse = MCPToolHandler::handle("workflow.execute",
        jsonToVar(nlohmann::json{{"workflow_run_id", result.workflowRunId.toStdString()}}),
        processor_,
        identity);
    auto executed = parseJsonResponse(executeResponse);
    result.rawResponse = executeResponse;
    result.response = executed;
    result.success = executed.value("success", false);

    if (result.success)
    {
        result.transactionId = findRollbackTransactionId(executed);
        lastWorkflowRunId_ = result.workflowRunId;
        lastRollbackTransactionId_ = result.transactionId;
        result.message = "WorkflowRun " + result.workflowRunId + " completed.\n" + plan.summary;
        return result;
    }

    const auto workflow = executed.value("workflow_run", nlohmann::json::object());
    const auto finalReport = workflow.value("finalReport", nlohmann::json::object());
    const auto workflowState = workflow.value("state", std::string{});
    const auto finalError = finalReport.value("error", executed.value("error", "workflow_execute_failed"));
    if (workflowState == "awaiting_approval" || finalError == "approval_required")
    {
        result.awaitingApproval = true;
        const auto approval = finalReport.value("approval_request", nlohmann::json::object());
        result.approvalId = juce::String(approval.value("id", std::string{}));
        result.message = "WorkflowRun " + result.workflowRunId + " is awaiting PermissionPolicy approval.";
        if (result.approvalId.isNotEmpty())
            result.message << "\nApprovalRequest " << result.approvalId << " is pending.";
        return result;
    }

    result.message = "WorkflowRun " + result.workflowRunId + " failed: "
        + juce::String(finalError);
    return result;
}

AssistantWorkflowResult AIAssistant::undoLastAssistantWorkflow()
{
    AssistantWorkflowResult result;
    result.handled = true;
    result.workflowRunId = lastWorkflowRunId_;
    result.transactionId = lastRollbackTransactionId_;

    if (lastWorkflowRunId_.isEmpty() || lastRollbackTransactionId_.isEmpty())
    {
        result.message = "No assistant workflow is available to undo.";
        return result;
    }

    const auto rollbackResponse = MCPToolHandler::handle("automation.rollback",
        jsonToVar(nlohmann::json{{"transaction_id", lastRollbackTransactionId_.toStdString()}}),
        processor_,
        processor_.getInstanceIdentity());
    auto parsed = parseJsonResponse(rollbackResponse);
    result.rawResponse = rollbackResponse;
    result.response = parsed;
    result.success = parsed.value("success", false);

    if (result.success)
    {
        const auto undoFeedbackResponse = MCPToolHandler::handle("memory.update_outcome_feedback",
            jsonToVar(nlohmann::json{
                {"transaction_id", lastRollbackTransactionId_.toStdString()},
                {"feedback_status", "undo"},
                {"user_feedback", "undo last assistant workflow"}
            }),
            processor_,
            processor_.getInstanceIdentity());
        auto undoFeedback = parseJsonResponse(undoFeedbackResponse);
        if (undoFeedback.value("success", false))
            result.response = undoFeedback;

        result.message = "WorkflowRun " + lastWorkflowRunId_
            + " rolled back via AutomationTransaction " + lastRollbackTransactionId_ + ".";
        lastWorkflowRunId_.clear();
        lastRollbackTransactionId_.clear();
        return result;
    }

    result.message = "Could not roll back assistant workflow " + lastWorkflowRunId_
        + ": " + juce::String(parsed.value("error", "rollback_failed"));
    return result;
}

AssistantWorkflowResult AIAssistant::recordFeedbackForLastWorkflow(const juce::String& text)
{
    AssistantWorkflowResult result;
    result.handled = true;
    result.workflowRunId = lastWorkflowRunId_;
    result.transactionId = lastRollbackTransactionId_;

    if (lastWorkflowRunId_.isEmpty() || lastRollbackTransactionId_.isEmpty())
    {
        result.message = "No assistant workflow is available to receive feedback.";
        return result;
    }

    const auto feedbackStatus = workflowFeedbackStatusFromText(text);

    const auto feedbackResponse = MCPToolHandler::handle("memory.update_outcome_feedback",
        jsonToVar(nlohmann::json{
            {"transaction_id", lastRollbackTransactionId_.toStdString()},
            {"feedback_status", feedbackStatus.toStdString()},
            {"user_feedback", text.trim().toStdString()}
        }),
        processor_,
        processor_.getInstanceIdentity());

    auto parsed = parseJsonResponse(feedbackResponse);
    result.rawResponse = feedbackResponse;
    result.response = parsed;
    result.success = parsed.value("success", false);

    if (result.success)
    {
        result.message = "Recorded feedback (" + feedbackStatus + ") for WorkflowRun " + lastWorkflowRunId_
            + " via AutomationTransaction " + lastRollbackTransactionId_ + ".";
        return result;
    }

    result.message = "Could not record feedback for assistant workflow " + lastWorkflowRunId_
        + ": " + juce::String(parsed.value("error", "memory_record_outcome_failed"));
    return result;
}

int AIAssistant::canonicalIndex(const ParamChange& change) noexcept
{
    return change.index >= 0 ? change.index : change.paramIndex;
}

} // namespace more_phi
