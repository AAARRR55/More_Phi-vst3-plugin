#pragma once

#include <juce_core/juce_core.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <vector>

namespace more_phi {

enum class RiskLevel
{
    ReadOnly,
    LowWrite,
    MediumWrite,
    HighImpact,
    External,
    Destructive
};

enum class AutonomyLevel
{
    Manual,
    Assist,
    CoPilot,
    Autopilot
};

enum class WorkflowState
{
    Draft,
    AwaitingApproval,
    Ready,
    Running,
    Verifying,
    Completed,
    Failed,
    Cancelled,
    RolledBack
};

enum class StepState
{
    Pending,
    Blocked,
    AwaitingApproval,
    Running,
    Verifying,
    Completed,
    Failed,
    Skipped,
    RolledBack
};

enum class MemoryScope
{
    Global,
    Project,
    Track,
    Plugin
};

juce::String toString(RiskLevel value);
juce::String toString(AutonomyLevel value);
juce::String toString(WorkflowState value);
juce::String toString(StepState value);
juce::String toString(MemoryScope value);

RiskLevel riskLevelFromString(const juce::String& value);
AutonomyLevel autonomyLevelFromString(const juce::String& value);
MemoryScope memoryScopeFromString(const juce::String& value);

struct WorkflowGoal
{
    juce::String id;
    juce::String userIntent;
    juce::String targetProfile;
    nlohmann::json successCriteria = nlohmann::json::object();
    nlohmann::json constraints = nlohmann::json::object();
    nlohmann::json contextSnapshot = nlohmann::json::object();
    juce::Time createdAt;
};

struct WorkflowStep
{
    juce::String id;
    juce::String toolName;
    nlohmann::json params = nlohmann::json::object();
    std::vector<juce::String> dependencies;
    nlohmann::json expectedObservation = nlohmann::json::object();
    nlohmann::json rollbackPlan = nlohmann::json::object();
    juce::String riskClass;
    int maxRetries = 1;
    int attemptCount = 0;
    StepState state = StepState::Pending;
};

struct RecoveryAttempt
{
    juce::String failedStepId;
    juce::String errorCode;
    nlohmann::json diagnosticSnapshot = nlohmann::json::object();
    nlohmann::json revisedStep = nlohmann::json::object();
    int attemptIndex = 0;
};

struct WorkflowRun
{
    juce::String id;
    WorkflowGoal goal;
    std::vector<WorkflowStep> steps;
    WorkflowState state = WorkflowState::Draft;
    uint64_t revision = 0;
    juce::Time createdAt;
    juce::Time updatedAt;
    nlohmann::json observations = nlohmann::json::object();
    nlohmann::json finalReport = nlohmann::json::object();
    std::vector<RecoveryAttempt> recoveryAttempts;
};

struct DawTransportContext
{
    bool playing = false;
    double bpm = 0.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    double ppqPosition = 0.0;
    double secondsPosition = 0.0;
    bool looping = false;
    bool available = false;
};

struct SessionContext
{
    juce::String instanceId;
    juce::String hostedPluginName;
    int hostedParameterCount = 0;
    DawTransportContext transport;
    nlohmann::json currentMeters = nlohmann::json::object();
    nlohmann::json currentSpectrum = nlohmann::json::object();
    nlohmann::json currentStereoField = nlohmann::json::object();
    nlohmann::json trackAssistantState = nlohmann::json::object();
};

struct PluginCapability
{
    juce::String id;
    juce::String confidence;
    nlohmann::json controls = nlohmann::json::array();
    nlohmann::json limits = nlohmann::json::object();
};

struct IntegrationEvent
{
    juce::String eventId;
    juce::String source;
    juce::String type;
    juce::String workflowRunId;
    juce::String transactionId;
    nlohmann::json payload = nlohmann::json::object();
    juce::Time timestamp;
    // Monotonic, gap-free per-bus sequence stamped by IntegrationEventBus::publish().
    // Used by BlackboardBridge to cursor forward without losing or re-delivering
    // events when the bus's bounded ring evicts old entries (audit C1).
    uint64_t sequence = 0;
};

struct SyncEnvelope
{
    juce::String instanceId;
    juce::String sessionId;
    uint64_t revision = 0;
    nlohmann::json statePatch = nlohmann::json::object();
    juce::String conflictPolicy = "merge";
};

struct MemoryRecord
{
    juce::String id;
    MemoryScope scope = MemoryScope::Global;
    juce::String subjectId;
    juce::String kind;
    nlohmann::json content = nlohmann::json::object();
    std::vector<float> embedding;
    float confidence = 0.5f;
    juce::Time createdAt;
    juce::Time lastUsedAt;
};

struct ActionOutcome
{
    juce::String actionId;
    juce::String workflowRunId;
    nlohmann::json beforeState = nlohmann::json::object();
    nlohmann::json afterState = nlohmann::json::object();
    nlohmann::json measurements = nlohmann::json::object();
    bool userAccepted = false;
    juce::String userFeedback;
    float outcomeScore = 0.0f;
    juce::String source;
    juce::String feedbackStatus;
};

struct OutcomeFeedbackUpdate
{
    juce::String actionId;
    juce::String feedbackStatus;
    juce::String userFeedback;
};

struct PermissionPolicy
{
    juce::String id;
    juce::String scope = "global";
    RiskLevel maxRisk = RiskLevel::LowWrite;
    bool requireApproval = true;
    int expiresAfterMinutes = 0;
    std::vector<juce::String> allowedTools;
};

struct ApprovalRequest
{
    juce::String id;
    juce::String workflowRunId;
    juce::String transactionId;
    juce::String toolName;
    nlohmann::json params = nlohmann::json::object();
    nlohmann::json predictedDiff = nlohmann::json::object();
    RiskLevel risk = RiskLevel::ReadOnly;
    juce::String explanation;
    juce::Time createdAt;
    juce::String status = "pending";
};

struct ParameterDiff
{
    int index = -1;
    juce::String name;
    float before = 0.0f;
    float after = 0.0f;
    juce::String semanticRole;
    RiskLevel risk = RiskLevel::LowWrite;
};

struct AutomationTransaction
{
    juce::String id;
    juce::String workflowRunId;
    juce::String workflowStepId;
    juce::String toolName;
    RiskLevel risk = RiskLevel::ReadOnly;
    nlohmann::json params = nlohmann::json::object();
    nlohmann::json beforeState = nlohmann::json::object();
    nlohmann::json afterState = nlohmann::json::object();
    nlohmann::json measurements = nlohmann::json::object();
    nlohmann::json result = nlohmann::json::object();
    nlohmann::json rollbackPlan = nlohmann::json::object();
    bool success = false;
    juce::String errorCode;
    juce::Time startedAt;
    juce::Time completedAt;
};

struct PermissionDecision
{
    bool allowed = false;
    RiskLevel risk = RiskLevel::ReadOnly;
    ApprovalRequest approval;
    juce::String reason;
};

nlohmann::json toJson(const WorkflowGoal& value);
nlohmann::json toJson(const WorkflowStep& value);
nlohmann::json toJson(const WorkflowRun& value);
nlohmann::json toJson(const RecoveryAttempt& value);
nlohmann::json toJson(const DawTransportContext& value);
nlohmann::json toJson(const SessionContext& value);
nlohmann::json toJson(const PluginCapability& value);
nlohmann::json toJson(const IntegrationEvent& value);
nlohmann::json toJson(const SyncEnvelope& value);
nlohmann::json toJson(const MemoryRecord& value);
nlohmann::json toJson(const ActionOutcome& value);
nlohmann::json toJson(const PermissionPolicy& value);
nlohmann::json toJson(const ApprovalRequest& value);
nlohmann::json toJson(const ParameterDiff& value);
nlohmann::json toJson(const AutomationTransaction& value);

WorkflowRun workflowRunFromJson(const nlohmann::json& value);
MemoryRecord memoryRecordFromJson(const nlohmann::json& value);
AutomationTransaction automationTransactionFromJson(const nlohmann::json& value);

nlohmann::json juceVarToJson(const juce::var& value);
juce::String makeAutomationId(const char* prefix);

class ActionLedger
{
public:
    explicit ActionLedger(juce::File storeDirectory = {});

    AutomationTransaction record(AutomationTransaction transaction);
    std::optional<AutomationTransaction> find(const juce::String& transactionId) const;
    nlohmann::json listRecent(int limit, const juce::String& workflowRunId = {}) const;
    nlohmann::json asJson() const;
    void clearForTests();

private:
    void load();
    void persist() const;

    juce::File directory_;
    juce::File file_;
    mutable juce::SpinLock mutex_;
    std::vector<AutomationTransaction> transactions_;
};

class PermissionKernel
{
public:
    explicit PermissionKernel(juce::File storeDirectory = {});

    void setAutonomyLevel(AutonomyLevel level);
    AutonomyLevel getAutonomyLevel() const;
    RiskLevel classifyTool(const juce::String& toolName, const nlohmann::json& params) const;
    PermissionDecision evaluate(const juce::String& toolName,
                                const nlohmann::json& params,
                                const juce::String& workflowRunId = {});
    nlohmann::json listApprovals() const;
    bool approve(const juce::String& approvalId);
    bool reject(const juce::String& approvalId);
    bool updateApprovalPreview(const juce::String& approvalId, const nlohmann::json& predictedDiff);
    nlohmann::json describeState() const;

private:
    bool isAllowedWithoutApproval(RiskLevel risk, const juce::String& toolName) const;
    void load();
    void persist() const;

    juce::File directory_;
    juce::File file_;
    mutable juce::SpinLock mutex_;
    AutonomyLevel autonomyLevel_ = AutonomyLevel::Assist;
    std::vector<ApprovalRequest> approvals_;
};

class IntegrationEventBus
{
public:
    explicit IntegrationEventBus(size_t capacity = 256);

    IntegrationEvent publish(IntegrationEvent event);
    nlohmann::json listRecent(int limit) const;
    // Returns events with sequence > sinceSequence, oldest-first (causal order),
    // capped at limit. Lets a cursor-based consumer advance forward without
    // re-delivering events even when the bounded ring has evicted old entries.
    nlohmann::json listRecentSince(uint64_t sinceSequence, int limit) const;
    // Highest sequence number published so far (0 before any event). A consumer
    // can cheaply check whether new work exists without pulling the full window.
    uint64_t lastSequence() const;
    SyncEnvelope exportState(const juce::String& instanceId, const juce::String& sessionId) const;
    IntegrationEvent applyEnvelope(const SyncEnvelope& envelope);
    uint64_t revision() const;

private:
    size_t capacity_ = 256;
    mutable juce::SpinLock mutex_;
    std::vector<IntegrationEvent> events_;
    uint64_t revision_ = 0;
    uint64_t sequenceCounter_ = 0;   // gap-free; advanced under mutex_ in publish()
};

class WorkflowOrchestrator
{
public:
    explicit WorkflowOrchestrator(juce::File storeDirectory = {});

    WorkflowRun createRun(const juce::String& userIntent, const nlohmann::json& context);
    WorkflowRun submitRun(WorkflowRun run);
    std::optional<WorkflowRun> getRun(const juce::String& runId) const;
    nlohmann::json listRuns() const;
    WorkflowRun executeRun(const juce::String& runId,
                           const std::function<nlohmann::json(const WorkflowRun&, const WorkflowStep&)>& executor = {});
    WorkflowRun cancelRun(const juce::String& runId);

private:
    std::optional<juce::String> validateGraph(const WorkflowRun& run) const;
    bool dependenciesComplete(const WorkflowRun& run, const WorkflowStep& step) const;
    void load();
    void persist() const;
    void replaceStoredRun(const WorkflowRun& run);
    nlohmann::json serializeRunsToJson() const;
    void persistInternal(const nlohmann::json& root) const;

    juce::File directory_;
    juce::File file_;

    mutable juce::SpinLock mutex_;
    std::vector<WorkflowRun> runs_;
};

class MemoryStore
{
public:
    explicit MemoryStore(juce::File storeDirectory = {});

    MemoryRecord remember(MemoryRecord record);
    MemoryRecord recordOutcome(ActionOutcome outcome);
    std::optional<MemoryRecord> updateOutcomeFeedback(OutcomeFeedbackUpdate update);
    nlohmann::json search(MemoryScope scope,
                          const juce::String& subjectId,
                          const juce::String& query,
                          int limit);
    nlohmann::json listOutcomes(const juce::String& workflowRunId = {}, int limit = 50) const;
    bool forget(const juce::String& id);
    nlohmann::json intentContext(const nlohmann::json& sessionContext, int limit);
    nlohmann::json describeState() const;
    void clearForTests();

private:
    void load();
    void persist() const;
    nlohmann::json serializeRecordsToJson() const;
    void persistInternal(const nlohmann::json& root) const;
    int scoreRecord(const MemoryRecord& record,
                    MemoryScope scope,
                    const juce::String& subjectId,
                    const juce::String& query) const;

    juce::File directory_;
    juce::File file_;
    mutable juce::SpinLock mutex_;
    std::vector<MemoryRecord> records_;
};

class AutomationRuntime
{
public:
    AutomationRuntime();

    ActionLedger& ledger() noexcept { return ledger_; }
    PermissionKernel& permissions() noexcept { return permissions_; }
    IntegrationEventBus& events() noexcept { return events_; }
    WorkflowOrchestrator& workflows() noexcept { return workflows_; }
    MemoryStore& memory() noexcept { return memory_; }

    static juce::File defaultStoreDirectory();

    static void setStoreDirectoryOverrideForTests(const juce::File& directory);
    static void clearStoreDirectoryOverrideForTests();

private:
    ActionLedger ledger_;
    PermissionKernel permissions_;
    IntegrationEventBus events_;
    WorkflowOrchestrator workflows_;
    MemoryStore memory_;
};

} // namespace more_phi
