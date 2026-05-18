#include "AutomationControlPlane.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>

namespace more_phi {

using json = nlohmann::json;

namespace {

std::mutex gStoreOverrideMutex;
std::optional<juce::File> gStoreDirectoryOverride;

juce::File resolveStoreDirectory()
{
    {
        std::lock_guard<std::mutex> lock(gStoreOverrideMutex);
        if (gStoreDirectoryOverride.has_value())
            return *gStoreDirectoryOverride;
    }

    return AutomationRuntime::defaultStoreDirectory();
}

std::string isoTime(const juce::Time& time)
{
    return time.toISO8601(true).toStdString();
}

juce::Time timeFromJson(const json& value)
{
    if (!value.is_string())
        return {};

    return juce::Time::fromISO8601(juce::String(value.get<std::string>()));
}

std::vector<juce::String> stringVectorFromJson(const json& value)
{
    std::vector<juce::String> out;
    if (!value.is_array())
        return out;

    out.reserve(value.size());
    for (const auto& item : value)
        if (item.is_string())
            out.push_back(juce::String(item.get<std::string>()));
    return out;
}

std::vector<float> floatVectorFromJson(const json& value)
{
    std::vector<float> out;
    if (!value.is_array())
        return out;

    out.reserve(value.size());
    for (const auto& item : value)
        if (item.is_number())
            out.push_back(item.get<float>());
    return out;
}

json stringVectorToJson(const std::vector<juce::String>& values)
{
    json out = json::array();
    for (const auto& value : values)
        out.push_back(value.toStdString());
    return out;
}

json riskMetadata(RiskLevel risk)
{
    return json{
        {"risk", toString(risk).toStdString()},
        {"risk_model", "static_tool_classification_v1"}
    };
}

bool nameStartsWith(const juce::String& value, const char* prefix)
{
    return value.startsWithIgnoreCase(prefix);
}

juce::String normalizeOutcomeFeedbackToken(const juce::String& status)
{
    auto text = status.trim().toLowerCase();
    text = text.replaceCharacter(' ', '_').replaceCharacter('-', '_');

    if (text == "approved" || text == "approve" || text == "accepted" || text == "accept")
        return "accepted";
    if (text == "rejected" || text == "reject")
        return "rejected";
    if (text == "too_much" || text == "overdid" || text == "overdone")
        return "too_much";
    if (text == "sounds_better" || text == "sound_better" || text == "better")
        return "sounds_better";
    if (text == "undo" || text == "undone" || text == "reversed")
        return "undo";

    return {};
}

bool outcomeStatusIsPositive(const juce::String& status)
{
    const auto normalized = normalizeOutcomeFeedbackToken(status);
    return normalized == "accepted" || normalized == "sounds_better";
}

float outcomeScoreForFeedbackStatus(const juce::String& status)
{
    const auto normalized = normalizeOutcomeFeedbackToken(status);
    if (normalized == "accepted") return 0.92f;
    if (normalized == "sounds_better") return 0.84f;
    if (normalized == "too_much") return 0.22f;
    if (normalized == "undo") return 0.05f;
    if (normalized == "rejected") return 0.10f;
    return 0.45f;
}

float confidenceForOutcome(const ActionOutcome& outcome)
{
    const auto normalizedScore = juce::jlimit(0.0f, 1.0f, outcome.outcomeScore);
    const auto status = normalizeOutcomeFeedbackToken(outcome.feedbackStatus);
    const auto baseConfidence = status == "accepted" ? 0.65f
        : (status == "sounds_better" ? 0.60f
        : (status == "too_much" ? 0.35f
        : (status == "undo" ? 0.30f
        : (status == "rejected" ? 0.25f : 0.40f))));
    const auto scoreWeight = status == "accepted" ? 0.30f
        : (status == "sounds_better" ? 0.25f
        : (status == "rejected" ? 0.10f : 0.20f));
    const auto feedbackBonus = outcome.userFeedback.isNotEmpty() ? 0.05f : 0.0f;
    return juce::jlimit(0.05f, 1.0f, baseConfidence + normalizedScore * scoreWeight + feedbackBonus);
}

ActionOutcome actionOutcomeFromJson(const json& value)
{
    ActionOutcome outcome;
    outcome.actionId = juce::String(value.value("actionId", ""));
    outcome.workflowRunId = juce::String(value.value("workflowRunId", ""));
    outcome.beforeState = value.value("beforeState", json::object());
    outcome.afterState = value.value("afterState", json::object());
    outcome.measurements = value.value("measurements", json::object());
    outcome.userAccepted = value.value("userAccepted", false);
    outcome.userFeedback = juce::String(value.value("userFeedback", ""));
    outcome.outcomeScore = value.value("outcomeScore", 0.0f);
    outcome.source = juce::String(value.value("source", ""));
    outcome.feedbackStatus = juce::String(value.value("feedbackStatus", ""));
    return outcome;
}

} // namespace

juce::String toString(RiskLevel value)
{
    switch (value)
    {
        case RiskLevel::ReadOnly: return "read_only";
        case RiskLevel::LowWrite: return "low_write";
        case RiskLevel::MediumWrite: return "medium_write";
        case RiskLevel::HighImpact: return "high_impact";
        case RiskLevel::External: return "external";
        case RiskLevel::Destructive: return "destructive";
    }
    return "read_only";
}

juce::String toString(AutonomyLevel value)
{
    switch (value)
    {
        case AutonomyLevel::Manual: return "manual";
        case AutonomyLevel::Assist: return "assist";
        case AutonomyLevel::CoPilot: return "co_pilot";
        case AutonomyLevel::Autopilot: return "autopilot";
    }
    return "assist";
}

juce::String toString(WorkflowState value)
{
    switch (value)
    {
        case WorkflowState::Draft: return "draft";
        case WorkflowState::AwaitingApproval: return "awaiting_approval";
        case WorkflowState::Ready: return "ready";
        case WorkflowState::Running: return "running";
        case WorkflowState::Verifying: return "verifying";
        case WorkflowState::Completed: return "completed";
        case WorkflowState::Failed: return "failed";
        case WorkflowState::Cancelled: return "cancelled";
        case WorkflowState::RolledBack: return "rolled_back";
    }
    return "draft";
}

juce::String toString(StepState value)
{
    switch (value)
    {
        case StepState::Pending: return "pending";
        case StepState::Blocked: return "blocked";
        case StepState::AwaitingApproval: return "awaiting_approval";
        case StepState::Running: return "running";
        case StepState::Verifying: return "verifying";
        case StepState::Completed: return "completed";
        case StepState::Failed: return "failed";
        case StepState::Skipped: return "skipped";
        case StepState::RolledBack: return "rolled_back";
    }
    return "pending";
}

juce::String toString(MemoryScope value)
{
    switch (value)
    {
        case MemoryScope::Global: return "global";
        case MemoryScope::Project: return "project";
        case MemoryScope::Track: return "track";
        case MemoryScope::Plugin: return "plugin";
    }
    return "global";
}

RiskLevel riskLevelFromString(const juce::String& value)
{
    const auto text = value.trim().toLowerCase();
    if (text == "low_write") return RiskLevel::LowWrite;
    if (text == "medium_write") return RiskLevel::MediumWrite;
    if (text == "high_impact") return RiskLevel::HighImpact;
    if (text == "external") return RiskLevel::External;
    if (text == "destructive") return RiskLevel::Destructive;
    return RiskLevel::ReadOnly;
}

AutonomyLevel autonomyLevelFromString(const juce::String& value)
{
    const auto text = value.trim().toLowerCase();
    if (text == "manual") return AutonomyLevel::Manual;
    if (text == "co_pilot" || text == "copilot") return AutonomyLevel::CoPilot;
    if (text == "autopilot") return AutonomyLevel::Autopilot;
    return AutonomyLevel::Assist;
}

MemoryScope memoryScopeFromString(const juce::String& value)
{
    const auto text = value.trim().toLowerCase();
    if (text == "project") return MemoryScope::Project;
    if (text == "track") return MemoryScope::Track;
    if (text == "plugin") return MemoryScope::Plugin;
    return MemoryScope::Global;
}

json toJson(const WorkflowGoal& value)
{
    return json{
        {"id", value.id.toStdString()},
        {"userIntent", value.userIntent.toStdString()},
        {"targetProfile", value.targetProfile.toStdString()},
        {"successCriteria", value.successCriteria},
        {"constraints", value.constraints},
        {"contextSnapshot", value.contextSnapshot},
        {"createdAt", isoTime(value.createdAt)}
    };
}

json toJson(const WorkflowStep& value)
{
    return json{
        {"id", value.id.toStdString()},
        {"toolName", value.toolName.toStdString()},
        {"params", value.params},
        {"dependencies", stringVectorToJson(value.dependencies)},
        {"expectedObservation", value.expectedObservation},
        {"rollbackPlan", value.rollbackPlan},
        {"riskClass", value.riskClass.toStdString()},
        {"maxRetries", value.maxRetries},
        {"attemptCount", value.attemptCount},
        {"state", toString(value.state).toStdString()}
    };
}

json toJson(const WorkflowRun& value)
{
    json steps = json::array();
    for (const auto& step : value.steps)
        steps.push_back(toJson(step));

    json recoveryAttempts = json::array();
    for (const auto& attempt : value.recoveryAttempts)
        recoveryAttempts.push_back(toJson(attempt));

    return json{
        {"id", value.id.toStdString()},
        {"goal", toJson(value.goal)},
        {"steps", steps},
        {"state", toString(value.state).toStdString()},
        {"revision", value.revision},
        {"createdAt", isoTime(value.createdAt)},
        {"updatedAt", isoTime(value.updatedAt)},
        {"observations", value.observations},
        {"finalReport", value.finalReport},
        {"recoveryAttempts", recoveryAttempts}
    };
}

json toJson(const RecoveryAttempt& value)
{
    return json{
        {"failedStepId", value.failedStepId.toStdString()},
        {"errorCode", value.errorCode.toStdString()},
        {"diagnosticSnapshot", value.diagnosticSnapshot},
        {"revisedStep", value.revisedStep},
        {"attemptIndex", value.attemptIndex}
    };
}

json toJson(const DawTransportContext& value)
{
    return json{
        {"available", value.available},
        {"playing", value.playing},
        {"bpm", value.bpm},
        {"timeSigNumerator", value.timeSigNumerator},
        {"timeSigDenominator", value.timeSigDenominator},
        {"ppqPosition", value.ppqPosition},
        {"secondsPosition", value.secondsPosition},
        {"looping", value.looping}
    };
}

json toJson(const SessionContext& value)
{
    return json{
        {"instanceId", value.instanceId.toStdString()},
        {"hostedPluginName", value.hostedPluginName.toStdString()},
        {"hostedParameterCount", value.hostedParameterCount},
        {"transport", toJson(value.transport)},
        {"currentMeters", value.currentMeters},
        {"currentSpectrum", value.currentSpectrum},
        {"currentStereoField", value.currentStereoField},
        {"trackAssistantState", value.trackAssistantState}
    };
}

json toJson(const PluginCapability& value)
{
    return json{
        {"id", value.id.toStdString()},
        {"confidence", value.confidence.toStdString()},
        {"controls", value.controls},
        {"limits", value.limits}
    };
}

json toJson(const IntegrationEvent& value)
{
    return json{
        {"eventId", value.eventId.toStdString()},
        {"source", value.source.toStdString()},
        {"type", value.type.toStdString()},
        {"workflowRunId", value.workflowRunId.toStdString()},
        {"transactionId", value.transactionId.toStdString()},
        {"payload", value.payload},
        {"timestamp", isoTime(value.timestamp)}
    };
}

json toJson(const SyncEnvelope& value)
{
    return json{
        {"instanceId", value.instanceId.toStdString()},
        {"sessionId", value.sessionId.toStdString()},
        {"revision", value.revision},
        {"statePatch", value.statePatch},
        {"conflictPolicy", value.conflictPolicy.toStdString()}
    };
}

json toJson(const MemoryRecord& value)
{
    return json{
        {"id", value.id.toStdString()},
        {"scope", toString(value.scope).toStdString()},
        {"subjectId", value.subjectId.toStdString()},
        {"kind", value.kind.toStdString()},
        {"content", value.content},
        {"embedding", value.embedding},
        {"confidence", value.confidence},
        {"createdAt", isoTime(value.createdAt)},
        {"lastUsedAt", isoTime(value.lastUsedAt)}
    };
}

json toJson(const ActionOutcome& value)
{
    return json{
        {"actionId", value.actionId.toStdString()},
        {"workflowRunId", value.workflowRunId.toStdString()},
        {"beforeState", value.beforeState},
        {"afterState", value.afterState},
        {"measurements", value.measurements},
        {"userAccepted", value.userAccepted},
        {"userFeedback", value.userFeedback.toStdString()},
        {"outcomeScore", value.outcomeScore},
        {"source", value.source.toStdString()},
        {"feedbackStatus", value.feedbackStatus.toStdString()}
    };
}

json toJson(const PermissionPolicy& value)
{
    return json{
        {"id", value.id.toStdString()},
        {"scope", value.scope.toStdString()},
        {"maxRisk", toString(value.maxRisk).toStdString()},
        {"requireApproval", value.requireApproval},
        {"expiresAfterMinutes", value.expiresAfterMinutes},
        {"allowedTools", stringVectorToJson(value.allowedTools)}
    };
}

json toJson(const ApprovalRequest& value)
{
    return json{
        {"id", value.id.toStdString()},
        {"workflowRunId", value.workflowRunId.toStdString()},
        {"transactionId", value.transactionId.toStdString()},
        {"toolName", value.toolName.toStdString()},
        {"params", value.params},
        {"predictedDiff", value.predictedDiff},
        {"risk", toString(value.risk).toStdString()},
        {"explanation", value.explanation.toStdString()},
        {"createdAt", isoTime(value.createdAt)},
        {"status", value.status.toStdString()}
    };
}

json toJson(const ParameterDiff& value)
{
    return json{
        {"index", value.index},
        {"name", value.name.toStdString()},
        {"before", value.before},
        {"after", value.after},
        {"semanticRole", value.semanticRole.toStdString()},
        {"risk", toString(value.risk).toStdString()}
    };
}

json toJson(const AutomationTransaction& value)
{
    return json{
        {"id", value.id.toStdString()},
        {"workflowRunId", value.workflowRunId.toStdString()},
        {"workflowStepId", value.workflowStepId.toStdString()},
        {"toolName", value.toolName.toStdString()},
        {"risk", toString(value.risk).toStdString()},
        {"params", value.params},
        {"beforeState", value.beforeState},
        {"afterState", value.afterState},
        {"measurements", value.measurements},
        {"result", value.result},
        {"rollbackPlan", value.rollbackPlan},
        {"success", value.success},
        {"errorCode", value.errorCode.toStdString()},
        {"startedAt", isoTime(value.startedAt)},
        {"completedAt", isoTime(value.completedAt)}
    };
}

WorkflowRun workflowRunFromJson(const json& value)
{
    WorkflowRun run;
    run.id = juce::String(value.value("id", ""));
    run.state = [] (const juce::String& text)
    {
        const auto lower = text.toLowerCase();
        if (lower == "awaiting_approval") return WorkflowState::AwaitingApproval;
        if (lower == "ready") return WorkflowState::Ready;
        if (lower == "running") return WorkflowState::Running;
        if (lower == "verifying") return WorkflowState::Verifying;
        if (lower == "completed") return WorkflowState::Completed;
        if (lower == "failed") return WorkflowState::Failed;
        if (lower == "cancelled") return WorkflowState::Cancelled;
        if (lower == "rolled_back") return WorkflowState::RolledBack;
        return WorkflowState::Draft;
    }(juce::String(value.value("state", "draft")));
    run.revision = value.value("revision", uint64_t{0});
    run.createdAt = timeFromJson(value.value("createdAt", json()));
    run.updatedAt = timeFromJson(value.value("updatedAt", json()));

    const auto& goal = value.contains("goal") ? value["goal"] : json::object();
    run.goal.id = juce::String(goal.value("id", ""));
    run.goal.userIntent = juce::String(goal.value("userIntent", ""));
    run.goal.targetProfile = juce::String(goal.value("targetProfile", ""));
    run.goal.successCriteria = goal.value("successCriteria", json::object());
    run.goal.constraints = goal.value("constraints", json::object());
    run.goal.contextSnapshot = goal.value("contextSnapshot", json::object());
    run.goal.createdAt = timeFromJson(goal.value("createdAt", json()));

    if (value.contains("steps") && value["steps"].is_array())
    {
        for (const auto& stepJson : value["steps"])
        {
            WorkflowStep step;
            step.id = juce::String(stepJson.value("id", ""));
            step.toolName = juce::String(stepJson.value("toolName", ""));
            step.params = stepJson.value("params", json::object());
            step.dependencies = stringVectorFromJson(stepJson.value("dependencies", json::array()));
            step.expectedObservation = stepJson.value("expectedObservation", json::object());
            step.rollbackPlan = stepJson.value("rollbackPlan", json::object());
            step.riskClass = juce::String(stepJson.value("riskClass", ""));
            step.maxRetries = stepJson.value("maxRetries", 1);
            step.attemptCount = stepJson.value("attemptCount", 0);

            const auto stateText = juce::String(stepJson.value("state", "pending")).toLowerCase();
            if (stateText == "blocked") step.state = StepState::Blocked;
            else if (stateText == "awaiting_approval") step.state = StepState::AwaitingApproval;
            else if (stateText == "running") step.state = StepState::Running;
            else if (stateText == "verifying") step.state = StepState::Verifying;
            else if (stateText == "completed") step.state = StepState::Completed;
            else if (stateText == "failed") step.state = StepState::Failed;
            else if (stateText == "skipped") step.state = StepState::Skipped;
            else if (stateText == "rolled_back") step.state = StepState::RolledBack;
            else step.state = StepState::Pending;

            run.steps.push_back(std::move(step));
        }
    }

    run.observations = value.value("observations", json::object());
    run.finalReport = value.value("finalReport", json::object());
    if (value.contains("recoveryAttempts") && value["recoveryAttempts"].is_array())
    {
        for (const auto& attemptJson : value["recoveryAttempts"])
        {
            RecoveryAttempt attempt;
            attempt.failedStepId = juce::String(attemptJson.value("failedStepId", ""));
            attempt.errorCode = juce::String(attemptJson.value("errorCode", ""));
            attempt.diagnosticSnapshot = attemptJson.value("diagnosticSnapshot", json::object());
            attempt.revisedStep = attemptJson.value("revisedStep", json::object());
            attempt.attemptIndex = attemptJson.value("attemptIndex", 0);
            run.recoveryAttempts.push_back(std::move(attempt));
        }
    }
    return run;
}

MemoryRecord memoryRecordFromJson(const json& value)
{
    MemoryRecord record;
    record.id = juce::String(value.value("id", ""));
    record.scope = memoryScopeFromString(juce::String(value.value("scope", "global")));
    record.subjectId = juce::String(value.value("subjectId", ""));
    record.kind = juce::String(value.value("kind", ""));
    record.content = value.value("content", json::object());
    record.embedding = floatVectorFromJson(value.value("embedding", json::array()));
    record.confidence = value.value("confidence", 0.5f);
    record.createdAt = timeFromJson(value.value("createdAt", json()));
    record.lastUsedAt = timeFromJson(value.value("lastUsedAt", json()));
    return record;
}

AutomationTransaction automationTransactionFromJson(const json& value)
{
    AutomationTransaction transaction;
    transaction.id = juce::String(value.value("id", ""));
    transaction.workflowRunId = juce::String(value.value("workflowRunId", ""));
    transaction.workflowStepId = juce::String(value.value("workflowStepId", ""));
    transaction.toolName = juce::String(value.value("toolName", ""));
    transaction.risk = riskLevelFromString(juce::String(value.value("risk", "read_only")));
    transaction.params = value.value("params", json::object());
    transaction.beforeState = value.value("beforeState", json::object());
    transaction.afterState = value.value("afterState", json::object());
    transaction.measurements = value.value("measurements", json::object());
    transaction.result = value.value("result", json::object());
    transaction.rollbackPlan = value.value("rollbackPlan", json::object());
    transaction.success = value.value("success", false);
    transaction.errorCode = juce::String(value.value("errorCode", ""));
    transaction.startedAt = timeFromJson(value.value("startedAt", json()));
    transaction.completedAt = timeFromJson(value.value("completedAt", json()));
    return transaction;
}

static ApprovalRequest approvalRequestFromJson(const json& value)
{
    ApprovalRequest approval;
    approval.id = juce::String(value.value("id", ""));
    approval.workflowRunId = juce::String(value.value("workflowRunId", ""));
    approval.transactionId = juce::String(value.value("transactionId", ""));
    approval.toolName = juce::String(value.value("toolName", ""));
    approval.params = value.value("params", json::object());
    approval.predictedDiff = value.value("predictedDiff", json::object());
    approval.risk = riskLevelFromString(juce::String(value.value("risk", "read_only")));
    approval.explanation = juce::String(value.value("explanation", ""));
    approval.createdAt = timeFromJson(value.value("createdAt", json()));
    approval.status = juce::String(value.value("status", "pending"));
    return approval;
}

json juceVarToJson(const juce::var& value)
{
    if (value.isVoid() || value.isUndefined())
        return nullptr;
    if (value.isBool())
        return static_cast<bool>(value);
    if (value.isInt())
        return static_cast<int>(value);
    if (value.isInt64())
        return static_cast<int64_t>(value);
    if (value.isDouble())
        return static_cast<double>(value);
    if (value.isString())
        return value.toString().toStdString();
    if (auto* array = value.getArray())
    {
        json out = json::array();
        for (const auto& item : *array)
            out.push_back(juceVarToJson(item));
        return out;
    }
    if (auto* object = value.getDynamicObject())
    {
        json out = json::object();
        for (const auto& property : object->getProperties())
            out[property.name.toString().toStdString()] = juceVarToJson(property.value);
        return out;
    }

    return value.toString().toStdString();
}

juce::String makeAutomationId(const char* prefix)
{
    return juce::String(prefix) + "_" + juce::Uuid().toString();
}

ActionLedger::ActionLedger(juce::File storeDirectory)
    : directory_(storeDirectory == juce::File{} ? resolveStoreDirectory() : storeDirectory),
      file_(directory_.getChildFile("action_ledger.json"))
{
    load();
}

AutomationTransaction ActionLedger::record(AutomationTransaction transaction)
{
    if (transaction.id.isEmpty())
        transaction.id = makeAutomationId("txn");
    if (transaction.startedAt == juce::Time{})
        transaction.startedAt = juce::Time::getCurrentTime();
    if (transaction.completedAt == juce::Time{})
        transaction.completedAt = juce::Time::getCurrentTime();

    std::lock_guard<std::mutex> lock(mutex_);
    transactions_.push_back(transaction);
    while (transactions_.size() > 256)
        transactions_.erase(transactions_.begin());
    persist();
    return transaction;
}

std::optional<AutomationTransaction> ActionLedger::find(const juce::String& transactionId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& transaction : transactions_)
        if (transaction.id == transactionId)
            return transaction;
    return std::nullopt;
}

json ActionLedger::listRecent(int limit, const juce::String& workflowRunId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int safeLimit = juce::jlimit(1, 512, limit <= 0 ? 50 : limit);
    json out = json::array();

    int emitted = 0;
    for (auto it = transactions_.rbegin(); it != transactions_.rend() && emitted < safeLimit; ++it)
    {
        if (workflowRunId.isNotEmpty() && it->workflowRunId != workflowRunId)
            continue;
        out.push_back(toJson(*it));
        ++emitted;
    }

    return out;
}

json ActionLedger::asJson() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (const auto& transaction : transactions_)
        out.push_back(toJson(transaction));
    return out;
}

void ActionLedger::clearForTests()
{
    std::lock_guard<std::mutex> lock(mutex_);
    transactions_.clear();
    persist();
}

void ActionLedger::load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    transactions_.clear();

    if (!file_.existsAsFile())
        return;

    // Older development builds wrote unbounded full-state ledgers. Rotate those
    // before parsing so first tool dispatch cannot stall the MCP socket thread.
    if (file_.getSize() > 1024 * 1024)
    {
        const auto rotated = directory_.getChildFile(
            "action_ledger_oversized_" + juce::String(juce::Time::currentTimeMillis()) + ".json");
        file_.moveFileTo(rotated);
        return;
    }

    try
    {
        const auto parsed = json::parse(file_.loadFileAsString().toStdString());
        if (!parsed.is_array())
            return;

        for (const auto& item : parsed)
            transactions_.push_back(automationTransactionFromJson(item));

        while (transactions_.size() > 256)
            transactions_.erase(transactions_.begin());
    }
    catch (...)
    {
        transactions_.clear();
    }
}

void ActionLedger::persist() const
{
    directory_.createDirectory();
    json out = json::array();
    for (const auto& transaction : transactions_)
        out.push_back(toJson(transaction));

    file_.replaceWithText(juce::String(out.dump(2)), false, false, "\n");
}

PermissionKernel::PermissionKernel(juce::File storeDirectory)
    : directory_(storeDirectory == juce::File{} ? resolveStoreDirectory() : storeDirectory),
      file_(directory_.getChildFile("permission_state.json"))
{
    load();
}

void PermissionKernel::setAutonomyLevel(AutonomyLevel level)
{
    std::lock_guard<std::mutex> lock(mutex_);
    autonomyLevel_ = level;
    persist();
}

AutonomyLevel PermissionKernel::getAutonomyLevel() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return autonomyLevel_;
}

RiskLevel PermissionKernel::classifyTool(const juce::String& toolName, const json& params) const
{
    const auto method = toolName.trim().toLowerCase();

    if (method == "izotope_ipc_dump")
        return RiskLevel::Destructive;

    if (nameStartsWith(method, "izotope_ipc_") || method == "ozone_run_assistant" || method == "sync.apply_envelope")
        return RiskLevel::External;

    if (method == "hosted_plugin.load" || method == "plugin_profile.save")
        return RiskLevel::HighImpact;

    if (method == "mastering.render_batch")
    {
        const bool dryRun = params.value("dry_run", true);
        return dryRun ? RiskLevel::ReadOnly : RiskLevel::HighImpact;
    }

    if (method == "capture_snapshot" || method == "recall_snapshot"
        || method == "hosted_plugin.capture_state" || method == "apply_mastering_plan"
        || method == "mastering.apply_plan" || method == "plugin_adapter.apply_action")
        return RiskLevel::MediumWrite;

    if (method == "set_parameter" || method == "set_parameters_batch"
        || method == "hosted_plugin.set_parameter" || method == "hosted_plugin.set_parameters"
        || method == "more_phi.set_parameter" || method == "more_phi.set_parameters"
        || method == "set_morph_position" || method == "mastering.select_candidate"
        || method == "plugin_profile.restore_safe_snapshot")
        return RiskLevel::LowWrite;

    if (method == "plugin_profile.apply_safe_action")
    {
        const bool dryRun = params.value("dry_run", false);
        return dryRun ? RiskLevel::ReadOnly : RiskLevel::LowWrite;
    }

    if (method == "memory.remember" || method == "memory.record_outcome"
        || method == "memory.update_outcome_feedback" || method == "memory.forget"
        || method == "automation.rollback"
        || method == "permission.set_autonomy" || method == "permission.approve"
        || method == "permission.reject" || method == "workflow.submit" || method == "workflow.execute"
        || method == "workflow.cancel")
        return RiskLevel::LowWrite;

    return RiskLevel::ReadOnly;
}

PermissionDecision PermissionKernel::evaluate(const juce::String& toolName,
                                              const json& params,
                                              const juce::String& workflowRunId)
{
    const auto risk = classifyTool(toolName, params);
    PermissionDecision decision;
    decision.risk = risk;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto approvalId = params.is_object() && params.contains("approval_id") && params["approval_id"].is_string()
        ? juce::String(params["approval_id"].get<std::string>())
        : juce::String();
    if (approvalId.isNotEmpty())
    {
        for (auto& approval : approvals_)
        {
            if (approval.id == approvalId
                && approval.status == "approved"
                && approval.toolName == toolName)
            {
                approval.status = "consumed";
                decision.allowed = true;
                decision.reason = "approved_request";
                persist();
                return decision;
            }
        }
    }

    if (isAllowedWithoutApproval(risk, toolName))
    {
        decision.allowed = true;
        decision.reason = "policy_allowed";
        return decision;
    }

    decision.allowed = false;
    decision.reason = "approval_required";
    decision.approval.id = makeAutomationId("approval");
    decision.approval.workflowRunId = workflowRunId;
    decision.approval.transactionId = makeAutomationId("txn");
    decision.approval.toolName = toolName;
    decision.approval.params = params;
    decision.approval.risk = risk;
    decision.approval.createdAt = juce::Time::getCurrentTime();
    decision.approval.explanation =
        "Dispatch-layer PermissionPolicy requires approval for "
        + toString(risk) + " tool " + toolName + ".";
    approvals_.push_back(decision.approval);
    while (approvals_.size() > 256)
        approvals_.erase(approvals_.begin());
    persist();
    return decision;
}

json PermissionKernel::listApprovals() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (const auto& approval : approvals_)
        out.push_back(toJson(approval));
    return out;
}

bool PermissionKernel::approve(const juce::String& approvalId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& approval : approvals_)
    {
        if (approval.id == approvalId && approval.status == "pending")
        {
            approval.status = "approved";
            persist();
            return true;
        }
    }
    return false;
}

bool PermissionKernel::reject(const juce::String& approvalId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& approval : approvals_)
    {
        if (approval.id == approvalId && approval.status == "pending")
        {
            approval.status = "rejected";
            persist();
            return true;
        }
    }
    return false;
}

bool PermissionKernel::updateApprovalPreview(const juce::String& approvalId, const json& predictedDiff)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& approval : approvals_)
    {
        if (approval.id == approvalId && approval.status == "pending")
        {
            approval.predictedDiff = predictedDiff;
            persist();
            return true;
        }
    }
    return false;
}

json PermissionKernel::describeState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    int pending = 0;
    for (const auto& approval : approvals_)
        if (approval.status == "pending")
            ++pending;

    return json{
        {"autonomy_level", toString(autonomyLevel_).toStdString()},
        {"policy_model", "system_enforced_dispatch_kernel_v1"},
        {"persistent", true},
        {"backend", "json_permission_state_v1"},
        {"store_path", file_.getFullPathName().toStdString()},
        {"approval_queue_size", pending},
        {"risk_defaults", json{
            {"manual", "read_only"},
            {"assist", "low_write"},
            {"co_pilot", "medium_write"},
            {"autopilot", "medium_write_plus_template_checkpoints"}
        }}
    };
}

bool PermissionKernel::isAllowedWithoutApproval(RiskLevel risk, const juce::String& toolName) const
{
    if (risk == RiskLevel::ReadOnly)
        return true;

    if (toolName == "permission.approve" || toolName == "permission.reject")
        return true;

    switch (autonomyLevel_)
    {
        case AutonomyLevel::Manual:
            return false;
        case AutonomyLevel::Assist:
            return risk == RiskLevel::LowWrite;
        case AutonomyLevel::CoPilot:
            return risk == RiskLevel::LowWrite || risk == RiskLevel::MediumWrite;
        case AutonomyLevel::Autopilot:
            return risk == RiskLevel::LowWrite || risk == RiskLevel::MediumWrite;
    }
    return false;
}

void PermissionKernel::load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    approvals_.clear();

    if (!file_.existsAsFile())
        return;

    if (file_.getSize() > 512 * 1024)
    {
        const auto rotated = directory_.getChildFile(
            "permission_state_oversized_" + juce::String(juce::Time::currentTimeMillis()) + ".json");
        file_.moveFileTo(rotated);
        return;
    }

    try
    {
        const auto parsed = json::parse(file_.loadFileAsString().toStdString());
        autonomyLevel_ = autonomyLevelFromString(juce::String(parsed.value("autonomy_level", "assist")));

        const auto approvals = parsed.value("approvals", json::array());
        if (approvals.is_array())
        {
            for (const auto& approval : approvals)
                approvals_.push_back(approvalRequestFromJson(approval));
        }

        while (approvals_.size() > 256)
            approvals_.erase(approvals_.begin());
    }
    catch (...)
    {
        approvals_.clear();
        autonomyLevel_ = AutonomyLevel::Assist;
    }
}

void PermissionKernel::persist() const
{
    directory_.createDirectory();

    json approvals = json::array();
    for (const auto& approval : approvals_)
        approvals.push_back(toJson(approval));

    const json root{
        {"schema_version", 1},
        {"backend", "json_permission_state_v1"},
        {"autonomy_level", toString(autonomyLevel_).toStdString()},
        {"approvals", approvals}
    };
    file_.replaceWithText(juce::String(root.dump(2)), false, false, "\n");
}

IntegrationEventBus::IntegrationEventBus(size_t capacity)
    : capacity_(std::max<size_t>(1, capacity))
{
}

IntegrationEvent IntegrationEventBus::publish(IntegrationEvent event)
{
    if (event.eventId.isEmpty())
        event.eventId = makeAutomationId("evt");
    if (event.timestamp == juce::Time{})
        event.timestamp = juce::Time::getCurrentTime();

    std::lock_guard<std::mutex> lock(mutex_);
    ++revision_;
    events_.push_back(event);
    while (events_.size() > capacity_)
        events_.erase(events_.begin());
    return event;
}

json IntegrationEventBus::listRecent(int limit) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int safeLimit = juce::jlimit(1, 512, limit <= 0 ? 50 : limit);
    json out = json::array();

    int emitted = 0;
    for (auto it = events_.rbegin(); it != events_.rend() && emitted < safeLimit; ++it, ++emitted)
        out.push_back(toJson(*it));
    return out;
}

SyncEnvelope IntegrationEventBus::exportState(const juce::String& instanceId, const juce::String& sessionId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    SyncEnvelope envelope;
    envelope.instanceId = instanceId;
    envelope.sessionId = sessionId;
    envelope.revision = revision_;
    envelope.conflictPolicy = "merge";
    envelope.statePatch = json{
        {"schema_version", 1},
        {"event_count", static_cast<int>(events_.size())},
        {"events", json::array()}
    };

    for (const auto& event : events_)
        envelope.statePatch["events"].push_back(toJson(event));
    return envelope;
}

IntegrationEvent IntegrationEventBus::applyEnvelope(const SyncEnvelope& envelope)
{
    IntegrationEvent event;
    event.source = "sync";
    event.type = "sync.envelope_applied";
    event.payload = toJson(envelope);
    return publish(std::move(event));
}

uint64_t IntegrationEventBus::revision() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return revision_;
}

WorkflowOrchestrator::WorkflowOrchestrator(juce::File storeDirectory)
    : directory_(storeDirectory == juce::File{} ? resolveStoreDirectory() : storeDirectory),
      file_(directory_.getChildFile("workflow_runs.json"))
{
    load();
}

WorkflowRun WorkflowOrchestrator::createRun(const juce::String& userIntent, const json& context)
{
    WorkflowRun run;
    run.id = makeAutomationId("workflow");
    run.goal.id = makeAutomationId("goal");
    run.goal.userIntent = userIntent;
    run.goal.targetProfile = context.value("targetProfile", "current_session");
    run.goal.contextSnapshot = context;
    run.goal.successCriteria = json{
        {"verification", "all_required_steps_completed"},
        {"partial_success_explicit", true}
    };
    run.goal.constraints = json{
        {"audio_thread", "never_execute_workflow_steps_on_audio_thread"},
        {"writes", "through_automation_dispatcher"}
    };
    run.goal.createdAt = juce::Time::getCurrentTime();
    run.state = WorkflowState::Draft;
    run.createdAt = run.goal.createdAt;
    run.updatedAt = run.createdAt;
    run.finalReport = json{
        {"planner_type", "deterministic_workflow_scaffold_v1"},
        {"llm_direct_tool_loop_replaced_for_workflows", true}
    };
    return submitRun(std::move(run));
}

WorkflowRun WorkflowOrchestrator::submitRun(WorkflowRun run)
{
    if (run.id.isEmpty())
        run.id = makeAutomationId("workflow");
    if (run.goal.id.isEmpty())
        run.goal.id = makeAutomationId("goal");
    if (run.goal.createdAt == juce::Time{})
        run.goal.createdAt = juce::Time::getCurrentTime();
    if (run.createdAt == juce::Time{})
        run.createdAt = run.goal.createdAt;
    run.updatedAt = juce::Time::getCurrentTime();

    int stepIndex = 0;
    for (auto& step : run.steps)
    {
        if (step.id.isEmpty())
            step.id = "step_" + juce::String(++stepIndex);
        if (step.maxRetries < 0)
            step.maxRetries = 0;
    }

    if (auto graphError = validateGraph(run))
    {
        run.state = WorkflowState::Failed;
        run.finalReport = json{
            {"success", false},
            {"error", "workflow_graph_invalid"},
            {"details", graphError->toStdString()}
        };
    }
    if (run.state == WorkflowState::Draft && run.steps.empty())
        run.state = WorkflowState::Ready;
    else if (run.state == WorkflowState::Draft)
        run.state = WorkflowState::Ready;

    std::lock_guard<std::mutex> lock(mutex_);
    ++run.revision;
    const auto existing = std::find_if(runs_.begin(), runs_.end(),
        [&run](const WorkflowRun& candidate) { return candidate.id == run.id; });
    if (existing != runs_.end())
        *existing = run;
    else
        runs_.push_back(run);
    persist();
    return run;
}

std::optional<WorkflowRun> WorkflowOrchestrator::getRun(const juce::String& runId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& run : runs_)
        if (run.id == runId)
            return run;
    return std::nullopt;
}

json WorkflowOrchestrator::listRuns() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    json out = json::array();
    for (const auto& run : runs_)
        out.push_back(toJson(run));
    return out;
}

WorkflowRun WorkflowOrchestrator::executeRun(const juce::String& runId,
                                             const std::function<json(const WorkflowRun&, const WorkflowStep&)>& executor)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(runs_.begin(), runs_.end(),
            [&runId](const WorkflowRun& candidate) { return candidate.id == runId; });

        if (it == runs_.end())
        {
            WorkflowRun missing;
            missing.id = runId;
            missing.state = WorkflowState::Failed;
            missing.finalReport = json{{"success", false}, {"error", "workflow_not_found"}};
            return missing;
        }

        if (it->state == WorkflowState::Cancelled || it->state == WorkflowState::Completed)
            return *it;
    }

    auto run = *getRun(runId);
    run.state = WorkflowState::Running;
    run.updatedAt = juce::Time::getCurrentTime();
    replaceStoredRun(run);

    if (run.steps.empty())
    {
        run.state = WorkflowState::Completed;
        run.updatedAt = juce::Time::getCurrentTime();
        run.finalReport = json{
            {"success", true},
            {"workflow_type", "scaffold_noop"},
            {"message", "WorkflowRun state machine accepted and completed an empty validated scaffold."}
        };
        replaceStoredRun(run);
        return run;
    }

    json observations = run.observations.value("steps", json::array());
    bool madeProgress = true;
    while (madeProgress)
    {
        madeProgress = false;
        for (auto& step : run.steps)
        {
            if (step.state == StepState::Completed
                || step.state == StepState::Skipped
                || step.state == StepState::RolledBack)
                continue;

            if (!dependenciesComplete(run, step))
            {
                step.state = StepState::Blocked;
                continue;
            }

            step.state = StepState::Running;
            ++step.attemptCount;
            run.updatedAt = juce::Time::getCurrentTime();
            replaceStoredRun(run);

            json result = executor ? executor(run, step) : json{{"success", false}, {"error", "executor_unavailable"}};
            observations.push_back(json{{"step_id", step.id.toStdString()}, {"result", result}});

            if (result.value("success", false))
            {
                step.state = StepState::Completed;
                madeProgress = true;
                run.updatedAt = juce::Time::getCurrentTime();
                replaceStoredRun(run);
                continue;
            }

            if (result.value("approval_required", false))
            {
                const auto approval = result.value("approval_request", json::object());
                if (approval.is_object() && approval.contains("id") && approval["id"].is_string())
                    step.params["approval_id"] = approval["id"].get<std::string>();

                step.state = StepState::AwaitingApproval;
                run.state = WorkflowState::AwaitingApproval;
                run.observations["steps"] = observations;
                run.updatedAt = juce::Time::getCurrentTime();
                run.finalReport = json{
                    {"success", false},
                    {"error", "approval_required"},
                    {"awaiting_step_id", step.id.toStdString()},
                    {"approval_request", approval}
                };
                replaceStoredRun(run);
                return run;
            }

            RecoveryAttempt recovery;
            recovery.failedStepId = step.id;
            recovery.errorCode = juce::String(result.value("error", "step_failed"));
            recovery.diagnosticSnapshot = json{
                {"step", toJson(step)},
                {"result", result},
                {"attempt_count", step.attemptCount},
                {"max_retries", step.maxRetries}
            };
            recovery.revisedStep = toJson(step);
            recovery.attemptIndex = step.attemptCount;
            run.recoveryAttempts.push_back(std::move(recovery));

            if (step.attemptCount <= step.maxRetries)
            {
                step.state = StepState::Pending;
                madeProgress = true;
                run.updatedAt = juce::Time::getCurrentTime();
                replaceStoredRun(run);
                continue;
            }

            step.state = StepState::Failed;
            run.state = WorkflowState::Failed;
            run.observations["steps"] = observations;
            run.updatedAt = juce::Time::getCurrentTime();
            run.finalReport = json{
                {"success", false},
                {"error", result.value("error", "step_failed")},
                {"failed_step_id", step.id.toStdString()},
                {"recovery_mode", "diagnose_revise_retry_v1"},
                {"attempts", step.attemptCount}
            };
            replaceStoredRun(run);
            return run;
        }
    }

    run.observations["steps"] = observations;
    const bool allComplete = std::all_of(run.steps.begin(), run.steps.end(), [] (const WorkflowStep& step)
    {
        return step.state == StepState::Completed || step.state == StepState::Skipped;
    });

    if (allComplete)
    {
        run.state = WorkflowState::Verifying;
        run.updatedAt = juce::Time::getCurrentTime();
        replaceStoredRun(run);

        run.state = WorkflowState::Completed;
        run.updatedAt = juce::Time::getCurrentTime();
        run.finalReport = json{
            {"success", true},
            {"verification", "all_steps_completed"}
        };
    }
    else
    {
        run.state = WorkflowState::Failed;
        run.updatedAt = juce::Time::getCurrentTime();
        run.finalReport = json{
            {"success", false},
            {"error", "workflow_blocked"},
            {"recovery_mode", "diagnose_revise_retry_v1"}
        };
    }

    replaceStoredRun(run);
    return run;
}

WorkflowRun WorkflowOrchestrator::cancelRun(const juce::String& runId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& run : runs_)
    {
        if (run.id == runId)
        {
            run.state = WorkflowState::Cancelled;
            run.updatedAt = juce::Time::getCurrentTime();
            ++run.revision;
            run.finalReport = json{{"success", false}, {"cancelled", true}};
            persist();
            return run;
        }
    }

    WorkflowRun missing;
    missing.id = runId;
    missing.state = WorkflowState::Failed;
    missing.finalReport = json{{"success", false}, {"error", "workflow_not_found"}};
    return missing;
}

std::optional<juce::String> WorkflowOrchestrator::validateGraph(const WorkflowRun& run) const
{
    std::map<std::string, size_t> indexById;
    for (size_t i = 0; i < run.steps.size(); ++i)
    {
        const auto id = run.steps[i].id.toStdString();
        if (id.empty())
            return juce::String("step_id_empty");
        if (indexById.find(id) != indexById.end())
            return juce::String("duplicate_step_id:") + run.steps[i].id;
        indexById[id] = i;
    }

    for (const auto& step : run.steps)
        for (const auto& dep : step.dependencies)
            if (indexById.find(dep.toStdString()) == indexById.end())
                return juce::String("missing_dependency:") + step.id + "->" + dep;

    std::set<std::string> visiting;
    std::set<std::string> visited;
    std::function<bool(const WorkflowStep&)> visit = [&] (const WorkflowStep& step)
    {
        const auto id = step.id.toStdString();
        if (visited.find(id) != visited.end())
            return true;
        if (visiting.find(id) != visiting.end())
            return false;

        visiting.insert(id);
        for (const auto& dep : step.dependencies)
        {
            const auto depIt = indexById.find(dep.toStdString());
            if (depIt == indexById.end())
                return false;
            if (!visit(run.steps[depIt->second]))
                return false;
        }

        visiting.erase(id);
        visited.insert(id);
        return true;
    };

    for (const auto& step : run.steps)
        if (!visit(step))
            return juce::String("cyclic_dependency:") + step.id;

    return std::nullopt;
}

bool WorkflowOrchestrator::dependenciesComplete(const WorkflowRun& run, const WorkflowStep& step) const
{
    for (const auto& dep : step.dependencies)
    {
        const auto it = std::find_if(run.steps.begin(), run.steps.end(),
            [&dep](const WorkflowStep& candidate) { return candidate.id == dep; });

        if (it == run.steps.end() || it->state != StepState::Completed)
            return false;
    }
    return true;
}

void WorkflowOrchestrator::load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    runs_.clear();

    if (!file_.existsAsFile())
        return;

    if (file_.getSize() > 1024 * 1024)
    {
        const auto rotated = directory_.getChildFile(
            "workflow_runs_oversized_" + juce::String(juce::Time::currentTimeMillis()) + ".json");
        file_.moveFileTo(rotated);
        return;
    }

    try
    {
        const auto parsed = json::parse(file_.loadFileAsString().toStdString());
        const auto runs = parsed.contains("runs") ? parsed["runs"] : parsed;
        if (!runs.is_array())
            return;

        for (const auto& item : runs)
            runs_.push_back(workflowRunFromJson(item));

        while (runs_.size() > 128)
            runs_.erase(runs_.begin());
    }
    catch (...)
    {
        runs_.clear();
    }
}

void WorkflowOrchestrator::persist() const
{
    directory_.createDirectory();
    json runs = json::array();
    for (const auto& run : runs_)
        runs.push_back(toJson(run));

    const json root{
        {"schema_version", 1},
        {"backend", "json_workflow_store_v1"},
        {"runs", runs}
    };
    file_.replaceWithText(juce::String(root.dump(2)), false, false, "\n");
}

void WorkflowOrchestrator::replaceStoredRun(const WorkflowRun& run)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto copy = run;
    ++copy.revision;
    const auto existing = std::find_if(runs_.begin(), runs_.end(),
        [&copy](const WorkflowRun& candidate) { return candidate.id == copy.id; });
    if (existing != runs_.end())
        *existing = copy;
    else
        runs_.push_back(copy);
    persist();
}

MemoryStore::MemoryStore(juce::File storeDirectory)
    : directory_(storeDirectory == juce::File{} ? resolveStoreDirectory() : storeDirectory),
      file_(directory_.getChildFile("memory_records.json"))
{
    load();
}

MemoryRecord MemoryStore::remember(MemoryRecord record)
{
    if (record.id.isEmpty())
        record.id = makeAutomationId("mem");
    if (record.createdAt == juce::Time{})
        record.createdAt = juce::Time::getCurrentTime();
    if (record.lastUsedAt == juce::Time{})
        record.lastUsedAt = record.createdAt;

    std::lock_guard<std::mutex> lock(mutex_);
    records_.push_back(record);
    persist();
    return record;
}

MemoryRecord MemoryStore::recordOutcome(ActionOutcome outcome)
{
    if (outcome.source.isEmpty())
        outcome.source = outcome.userFeedback.isNotEmpty() ? "user_feedback" : "automatic_transaction";
    if (outcome.feedbackStatus.isEmpty())
        outcome.feedbackStatus = outcome.userFeedback.isNotEmpty()
            ? (outcome.userAccepted ? "accepted" : "rejected")
            : "unreviewed";

    MemoryRecord record;
    record.scope = MemoryScope::Project;
    record.subjectId = outcome.workflowRunId;
    record.kind = "action_outcome";
    record.content = toJson(outcome);
    record.confidence = confidenceForOutcome(outcome);
    record.createdAt = juce::Time::getCurrentTime();
    record.lastUsedAt = record.createdAt;

    std::lock_guard<std::mutex> lock(mutex_);
    if (outcome.actionId.isNotEmpty())
    {
        const auto actionId = outcome.actionId.toStdString();
        auto existing = std::find_if(records_.begin(), records_.end(),
            [&actionId](const MemoryRecord& item)
            {
                return item.kind == "action_outcome"
                    && item.content.is_object()
                    && item.content.value("actionId", "") == actionId;
            });

        if (existing != records_.end())
        {
            record.id = existing->id;
            record.createdAt = existing->createdAt == juce::Time{} ? record.createdAt : existing->createdAt;
            *existing = record;
            persist();
            return *existing;
        }
    }

    record.id = makeAutomationId("mem");
    records_.push_back(record);
    persist();
    return record;
}

std::optional<MemoryRecord> MemoryStore::updateOutcomeFeedback(OutcomeFeedbackUpdate update)
{
    const auto normalizedStatus = normalizeOutcomeFeedbackToken(update.feedbackStatus);
    if (update.actionId.isEmpty() || normalizedStatus.isEmpty())
        return std::nullopt;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto actionId = update.actionId.toStdString();
    auto existing = std::find_if(records_.begin(), records_.end(),
        [&actionId](const MemoryRecord& item)
        {
            return item.kind == "action_outcome"
                && item.content.is_object()
                && item.content.value("actionId", "") == actionId;
        });

    if (existing == records_.end())
        return std::nullopt;

    auto outcome = actionOutcomeFromJson(existing->content);
    outcome.feedbackStatus = normalizedStatus;
    outcome.userAccepted = outcomeStatusIsPositive(normalizedStatus);
    outcome.userFeedback = update.userFeedback;
    outcome.outcomeScore = outcomeScoreForFeedbackStatus(normalizedStatus);
    outcome.source = normalizedStatus == "undo" ? "undo_feedback" : "user_feedback";

    existing->content = toJson(outcome);
    existing->confidence = confidenceForOutcome(outcome);
    existing->lastUsedAt = juce::Time::getCurrentTime();
    persist();
    return *existing;
}

json MemoryStore::search(MemoryScope scope,
                         const juce::String& subjectId,
                         const juce::String& query,
                         int limit)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int safeLimit = juce::jlimit(1, 128, limit <= 0 ? 10 : limit);

    struct ScoredRecord
    {
        int score = 0;
        MemoryRecord record;
    };

    std::vector<ScoredRecord> scored;
    for (auto& record : records_)
    {
        const int score = scoreRecord(record, scope, subjectId, query);
        if (score > 0)
        {
            record.lastUsedAt = juce::Time::getCurrentTime();
            scored.push_back({score, record});
        }
    }

    std::sort(scored.begin(), scored.end(), [] (const ScoredRecord& a, const ScoredRecord& b)
    {
        if (a.score != b.score)
            return a.score > b.score;
        return a.record.confidence > b.record.confidence;
    });

    json out = json::array();
    for (int i = 0; i < static_cast<int>(scored.size()) && i < safeLimit; ++i)
    {
        auto item = toJson(scored[static_cast<size_t>(i)].record);
        item["retrieval_score"] = scored[static_cast<size_t>(i)].score;
        out.push_back(item);
    }

    persist();
    return out;
}

json MemoryStore::listOutcomes(const juce::String& workflowRunId, int limit) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int safeLimit = juce::jlimit(1, 256, limit <= 0 ? 50 : limit);

    std::vector<MemoryRecord> matches;
    for (const auto& record : records_)
    {
        if (record.kind != "action_outcome")
            continue;

        const auto contentWorkflowRunId = juce::String(record.content.value("workflowRunId", ""));
        if (workflowRunId.isNotEmpty() && record.subjectId != workflowRunId && contentWorkflowRunId != workflowRunId)
            continue;

        matches.push_back(record);
    }

    std::sort(matches.begin(), matches.end(), [] (const MemoryRecord& a, const MemoryRecord& b)
    {
        return a.createdAt.toMilliseconds() > b.createdAt.toMilliseconds();
    });

    json out = json::array();
    for (int i = 0; i < static_cast<int>(matches.size()) && i < safeLimit; ++i)
        out.push_back(toJson(matches[static_cast<size_t>(i)]));
    return out;
}

bool MemoryStore::forget(const juce::String& id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto oldSize = records_.size();
    records_.erase(std::remove_if(records_.begin(), records_.end(),
        [&id](const MemoryRecord& record) { return record.id == id; }), records_.end());

    const bool removed = records_.size() != oldSize;
    if (removed)
        persist();
    return removed;
}

json MemoryStore::intentContext(const json& sessionContext, int limit)
{
    const auto pluginName = juce::String(sessionContext.value("hostedPluginName", ""));
    const auto trackId = juce::String(sessionContext.value("trackId", ""));

    json results = json::array();
    auto appendResults = [&results](const json& items)
    {
        for (const auto& item : items)
            results.push_back(item);
    };

    appendResults(search(MemoryScope::Track, trackId, "", limit));
    appendResults(search(MemoryScope::Plugin, pluginName, "", limit));
    appendResults(search(MemoryScope::Project, "", "", limit));
    appendResults(search(MemoryScope::Global, "", "", limit));

    return json{
        {"backend", "json_local_store_v1"},
        {"vector_index_loaded", false},
        {"retrieval_order", json::array({"track", "plugin", "project", "global"})},
        {"records", results}
    };
}

json MemoryStore::describeState() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return json{
        {"backend", "json_local_store_v1"},
        {"sqlite_backend_loaded", false},
        {"vector_index_loaded", false},
        {"record_count", static_cast<int>(records_.size())},
        {"store_path", file_.getFullPathName().toStdString()}
    };
}

void MemoryStore::clearForTests()
{
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    persist();
}

void MemoryStore::load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();

    if (!file_.existsAsFile())
        return;

    try
    {
        const auto parsed = json::parse(file_.loadFileAsString().toStdString());
        const auto records = parsed.contains("records") ? parsed["records"] : parsed;
        if (!records.is_array())
            return;

        for (const auto& item : records)
            records_.push_back(memoryRecordFromJson(item));
    }
    catch (...)
    {
        records_.clear();
    }
}

void MemoryStore::persist() const
{
    directory_.createDirectory();
    json records = json::array();
    for (const auto& record : records_)
        records.push_back(toJson(record));

    const json root{
        {"schema_version", 1},
        {"backend", "json_local_store_v1"},
        {"vector_index_loaded", false},
        {"records", records}
    };
    file_.replaceWithText(juce::String(root.dump(2)), false, false, "\n");
}

int MemoryStore::scoreRecord(const MemoryRecord& record,
                             MemoryScope scope,
                             const juce::String& subjectId,
                             const juce::String& query) const
{
    int score = 0;
    if (record.scope == scope)
        score += 20;
    if (subjectId.isNotEmpty() && record.subjectId == subjectId)
        score += 30;
    if (subjectId.isEmpty())
        score += 1;

    if (query.isEmpty())
        score += 1;
    else
    {
        const auto haystack = juce::String(toJson(record).dump()).toLowerCase();
        for (const auto token : juce::StringArray::fromTokens(query.toLowerCase(), " ", ""))
            if (token.isNotEmpty() && haystack.contains(token))
                score += 10;
    }

    if (record.confidence > 0.75f)
        score += 2;
    return score;
}

AutomationRuntime::AutomationRuntime()
    : ledger_(resolveStoreDirectory()),
      permissions_(resolveStoreDirectory()),
      events_(256),
      workflows_(resolveStoreDirectory()),
      memory_(resolveStoreDirectory())
{
}

juce::File AutomationRuntime::defaultStoreDirectory()
{
    const auto executableName = juce::File::getSpecialLocation(juce::File::currentExecutableFile)
        .getFileNameWithoutExtension()
        .toLowerCase();
    const bool runningTestHost = executableName == "morephitests" || executableName == "morephimcpservertests";
    if (runningTestHost)
    {
        static const auto testDirectory = []
        {
            auto directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
                .getChildFile("More-Phi")
                .getChildFile("automation-test-runtime")
                .getChildFile(juce::String::toHexString(juce::Time::getHighResolutionTicks())
                    + "_" + juce::String(juce::Time::currentTimeMillis()));
            directory.deleteRecursively();
            return directory;
        }();
        return testDirectory;
    }

    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("More-Phi")
        .getChildFile("automation");
}

void AutomationRuntime::setStoreDirectoryOverrideForTests(const juce::File& directory)
{
    std::lock_guard<std::mutex> lock(gStoreOverrideMutex);
    gStoreDirectoryOverride = directory;
}

void AutomationRuntime::clearStoreDirectoryOverrideForTests()
{
    std::lock_guard<std::mutex> lock(gStoreOverrideMutex);
    gStoreDirectoryOverride.reset();
}

} // namespace more_phi
