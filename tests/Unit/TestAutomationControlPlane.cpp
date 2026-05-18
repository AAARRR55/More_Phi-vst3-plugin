#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "AI/AutomationControlPlane.h"

namespace {

struct ScopedAutomationStore
{
    ScopedAutomationStore()
    {
        directory = juce::File::getSpecialLocation(juce::File::tempDirectory)
            .getNonexistentChildFile("morephi_automation_control_plane_unit", "");
        directory.createDirectory();
        more_phi::AutomationRuntime::setStoreDirectoryOverrideForTests(directory);
    }

    ~ScopedAutomationStore()
    {
        more_phi::AutomationRuntime::clearStoreDirectoryOverrideForTests();
        directory.deleteRecursively();
    }

    juce::File directory;
};

} // namespace

TEST_CASE("PermissionKernel enforces autonomy risk boundaries", "[automation][permission]")
{
    ScopedAutomationStore scoped;
    more_phi::PermissionKernel permissions(scoped.directory);

    auto low = permissions.evaluate("set_parameter", nlohmann::json{{"index", 1}, {"value", 0.5}});
    REQUIRE(low.allowed);
    REQUIRE(low.risk == more_phi::RiskLevel::LowWrite);

    auto high = permissions.evaluate("hosted_plugin.load", nlohmann::json{{"path", "C:/missing.vst3"}});
    REQUIRE_FALSE(high.allowed);
    REQUIRE(high.risk == more_phi::RiskLevel::HighImpact);
    REQUIRE(high.approval.status == "pending");

    permissions.setAutonomyLevel(more_phi::AutonomyLevel::CoPilot);
    auto medium = permissions.evaluate("capture_snapshot", nlohmann::json{{"slot", 0}});
    REQUIRE(medium.allowed);
    REQUIRE(medium.risk == more_phi::RiskLevel::MediumWrite);

    auto external = permissions.evaluate("sync.apply_envelope", nlohmann::json::object());
    REQUIRE_FALSE(external.allowed);
    REQUIRE(external.risk == more_phi::RiskLevel::External);
}

TEST_CASE("PermissionKernel persists autonomy and approval queue state", "[automation][permission]")
{
    ScopedAutomationStore scoped;
    more_phi::PermissionKernel permissions(scoped.directory);
    permissions.setAutonomyLevel(more_phi::AutonomyLevel::Manual);

    auto blocked = permissions.evaluate("set_parameter", nlohmann::json{{"index", 1}, {"value", 0.25}});
    REQUIRE_FALSE(blocked.allowed);
    REQUIRE(blocked.approval.id.isNotEmpty());

    const auto preview = nlohmann::json{{"diffs", nlohmann::json::array({{{"index", 1}, {"before", 0.5}, {"after", 0.25}}})}};
    REQUIRE(permissions.updateApprovalPreview(blocked.approval.id, preview));
    REQUIRE(permissions.approve(blocked.approval.id));

    more_phi::PermissionKernel reloaded(scoped.directory);
    const auto state = reloaded.describeState();
    REQUIRE(state["autonomy_level"].get<std::string>() == "manual");
    REQUIRE(state["persistent"].get<bool>());

    const auto approvals = reloaded.listApprovals();
    REQUIRE(approvals.is_array());
    REQUIRE_FALSE(approvals.empty());
    REQUIRE(approvals[0]["id"].get<std::string>() == blocked.approval.id.toStdString());
    REQUIRE(approvals[0]["status"].get<std::string>() == "approved");
    REQUIRE(approvals[0]["predictedDiff"]["diffs"][0]["after"].get<double>() == 0.25);
}

TEST_CASE("ActionLedger records auditable AutomationTransaction entries", "[automation][ledger]")
{
    ScopedAutomationStore scoped;
    more_phi::ActionLedger ledger(scoped.directory);

    more_phi::AutomationTransaction transaction;
    transaction.workflowRunId = "workflow-test";
    transaction.workflowStepId = "step-test";
    transaction.toolName = "set_parameter";
    transaction.risk = more_phi::RiskLevel::LowWrite;
    transaction.success = true;
    transaction.beforeState = nlohmann::json{{"value", 0.1}};
    transaction.afterState = nlohmann::json{{"value", 0.2}};

    const auto stored = ledger.record(transaction);
    REQUIRE(stored.id.isNotEmpty());

    const auto found = ledger.find(stored.id);
    REQUIRE(found.has_value());
    REQUIRE(found->toolName == "set_parameter");
    REQUIRE(found->workflowRunId == "workflow-test");
    REQUIRE(found->workflowStepId == "step-test");
    REQUIRE(found->success);

    const auto recent = ledger.listRecent(5);
    REQUIRE(recent.is_array());
    REQUIRE(recent.size() == 1);
    REQUIRE(recent[0]["id"].get<std::string>() == stored.id.toStdString());
    REQUIRE(recent[0]["workflowStepId"].get<std::string>() == "step-test");

    const auto filtered = ledger.listRecent(5, "workflow-test");
    REQUIRE(filtered.size() == 1);
    const auto empty = ledger.listRecent(5, "other-workflow");
    REQUIRE(empty.empty());
}

TEST_CASE("WorkflowOrchestrator creates and completes durable WorkflowRun scaffolds", "[automation][workflow]")
{
    more_phi::WorkflowOrchestrator orchestrator;

    auto run = orchestrator.createRun("prepare a streaming master", nlohmann::json{{"targetProfile", "streaming"}});
    REQUIRE(run.id.isNotEmpty());
    REQUIRE(run.state == more_phi::WorkflowState::Ready);
    REQUIRE(run.goal.userIntent == "prepare a streaming master");

    auto executed = orchestrator.executeRun(run.id);
    REQUIRE(executed.state == more_phi::WorkflowState::Completed);
    REQUIRE(executed.finalReport["success"].get<bool>());

    const auto fetched = orchestrator.getRun(run.id);
    REQUIRE(fetched.has_value());
    REQUIRE(fetched->state == more_phi::WorkflowState::Completed);
}

TEST_CASE("WorkflowOrchestrator persists submitted DAGs and executes dependencies", "[automation][workflow]")
{
    ScopedAutomationStore scoped;
    more_phi::WorkflowOrchestrator orchestrator(scoped.directory);

    more_phi::WorkflowRun run;
    run.goal.userIntent = "apply a verified morph move";

    more_phi::WorkflowStep apply;
    apply.id = "apply";
    apply.toolName = "set_morph_position";
    apply.dependencies = {"capture"};
    apply.params = nlohmann::json{{"x", 0.25}};

    more_phi::WorkflowStep capture;
    capture.id = "capture";
    capture.toolName = "context.get_session";

    run.steps = {apply, capture};

    const auto submitted = orchestrator.submitRun(run);
    REQUIRE(submitted.state == more_phi::WorkflowState::Ready);

    std::vector<std::string> executionOrder;
    const auto executed = orchestrator.executeRun(submitted.id,
        [&executionOrder](const more_phi::WorkflowRun&, const more_phi::WorkflowStep& step)
        {
            executionOrder.push_back(step.id.toStdString());
            return nlohmann::json{{"success", true}};
        });

    REQUIRE(executed.state == more_phi::WorkflowState::Completed);
    REQUIRE(executionOrder.size() == 2);
    REQUIRE(executionOrder[0] == "capture");
    REQUIRE(executionOrder[1] == "apply");
    REQUIRE(executed.observations["steps"].size() == 2);

    more_phi::WorkflowOrchestrator reloaded(scoped.directory);
    const auto persisted = reloaded.getRun(submitted.id);
    REQUIRE(persisted.has_value());
    REQUIRE(persisted->state == more_phi::WorkflowState::Completed);
    REQUIRE(persisted->steps[0].state == more_phi::StepState::Completed);
}

TEST_CASE("WorkflowOrchestrator rejects invalid workflow graphs", "[automation][workflow]")
{
    ScopedAutomationStore scoped;
    more_phi::WorkflowOrchestrator orchestrator(scoped.directory);

    more_phi::WorkflowRun missingDep;
    missingDep.goal.userIntent = "invalid missing dependency";
    more_phi::WorkflowStep step;
    step.id = "apply";
    step.toolName = "set_morph_position";
    step.dependencies = {"missing"};
    missingDep.steps = {step};

    const auto rejectedMissing = orchestrator.submitRun(missingDep);
    REQUIRE(rejectedMissing.state == more_phi::WorkflowState::Failed);
    REQUIRE(rejectedMissing.finalReport["error"].get<std::string>() == "workflow_graph_invalid");

    more_phi::WorkflowRun cyclic;
    cyclic.goal.userIntent = "invalid cycle";
    more_phi::WorkflowStep first;
    first.id = "first";
    first.toolName = "context.get_session";
    first.dependencies = {"second"};
    more_phi::WorkflowStep second;
    second.id = "second";
    second.toolName = "context.get_transport";
    second.dependencies = {"first"};
    cyclic.steps = {first, second};

    const auto rejectedCycle = orchestrator.submitRun(cyclic);
    REQUIRE(rejectedCycle.state == more_phi::WorkflowState::Failed);
    REQUIRE(rejectedCycle.finalReport["details"].get<std::string>().find("cyclic_dependency") != std::string::npos);
}

TEST_CASE("WorkflowOrchestrator records Diagnose-Revise-Retry attempts", "[automation][workflow]")
{
    ScopedAutomationStore scoped;
    more_phi::WorkflowOrchestrator orchestrator(scoped.directory);

    more_phi::WorkflowRun run;
    run.goal.userIntent = "retry transient failure";
    more_phi::WorkflowStep step;
    step.id = "apply";
    step.toolName = "set_morph_position";
    step.maxRetries = 1;
    run.steps = {step};

    const auto submitted = orchestrator.submitRun(run);
    int calls = 0;
    const auto executed = orchestrator.executeRun(submitted.id,
        [&calls](const more_phi::WorkflowRun&, const more_phi::WorkflowStep&)
        {
            ++calls;
            if (calls == 1)
                return nlohmann::json{{"success", false}, {"error", "transient_queue_busy"}};
            return nlohmann::json{{"success", true}};
        });

    REQUIRE(executed.state == more_phi::WorkflowState::Completed);
    REQUIRE(calls == 2);
    REQUIRE(executed.recoveryAttempts.size() == 1);
    REQUIRE(executed.recoveryAttempts[0].errorCode == "transient_queue_busy");
    REQUIRE(executed.steps[0].attemptCount == 2);
}

TEST_CASE("IntegrationEventBus preserves ordering within bounded history", "[automation][events]")
{
    more_phi::IntegrationEventBus bus(2);

    bus.publish(more_phi::IntegrationEvent{{}, "test", "first", {}, {}, {}, juce::Time::getCurrentTime()});
    bus.publish(more_phi::IntegrationEvent{{}, "test", "second", {}, {}, {}, juce::Time::getCurrentTime()});
    bus.publish(more_phi::IntegrationEvent{{}, "test", "third", {}, {}, {}, juce::Time::getCurrentTime()});

    const auto recent = bus.listRecent(10);
    REQUIRE(recent.size() == 2);
    REQUIRE(recent[0]["type"].get<std::string>() == "third");
    REQUIRE(recent[1]["type"].get<std::string>() == "second");
    REQUIRE(bus.revision() == 3);
}

TEST_CASE("MemoryStore supports CRUD and scoped lexical retrieval", "[automation][memory]")
{
    ScopedAutomationStore scoped;
    more_phi::MemoryStore memory(scoped.directory);

    more_phi::MemoryRecord record;
    record.scope = more_phi::MemoryScope::Plugin;
    record.subjectId = "Ozone";
    record.kind = "preference";
    record.content = nlohmann::json{{"text", "Prefer conservative limiter ceiling"}};
    record.confidence = 0.9f;

    const auto stored = memory.remember(record);
    REQUIRE(stored.id.isNotEmpty());

    const auto results = memory.search(more_phi::MemoryScope::Plugin, "Ozone", "limiter ceiling", 5);
    REQUIRE(results.is_array());
    REQUIRE_FALSE(results.empty());
    REQUIRE(results[0]["id"].get<std::string>() == stored.id.toStdString());

    REQUIRE(memory.forget(stored.id));
    const auto empty = memory.search(more_phi::MemoryScope::Plugin, "Ozone", "limiter", 5);
    REQUIRE(empty.empty());
}

TEST_CASE("MemoryStore updates ActionOutcome feedback without duplicating records", "[automation][memory]")
{
    ScopedAutomationStore scoped;
    more_phi::MemoryStore memory(scoped.directory);

    more_phi::ActionOutcome outcome;
    outcome.actionId = "txn-feedback-dedup-test";
    outcome.workflowRunId = "workflow-feedback-dedup-test";
    outcome.beforeState = nlohmann::json{{"morph_x", 0.2}};
    outcome.afterState = nlohmann::json{{"morph_x", 0.4}};
    outcome.measurements = nlohmann::json{{"more_phi_morph_x_delta", 0.2}};
    outcome.outcomeScore = 0.55f;
    outcome.source = "automatic_transaction";
    outcome.feedbackStatus = "unreviewed";

    const auto stored = memory.recordOutcome(outcome);
    REQUIRE(stored.id.isNotEmpty());
    REQUIRE(stored.content["feedbackStatus"].get<std::string>() == "unreviewed");

    outcome.afterState = nlohmann::json{{"morph_x", 0.45}};
    const auto duplicate = memory.recordOutcome(outcome);
    REQUIRE(duplicate.id == stored.id);
    REQUIRE(memory.listOutcomes("workflow-feedback-dedup-test", 10).size() == 1);

    more_phi::OutcomeFeedbackUpdate feedback;
    feedback.actionId = outcome.actionId;
    feedback.feedbackStatus = "sounds better";
    feedback.userFeedback = "keep this gentler morph range";

    const auto updated = memory.updateOutcomeFeedback(feedback);
    REQUIRE(updated.has_value());
    REQUIRE(updated->id == stored.id);
    REQUIRE(updated->content["actionId"].get<std::string>() == outcome.actionId.toStdString());
    REQUIRE(updated->content["feedbackStatus"].get<std::string>() == "sounds_better");
    REQUIRE(updated->content["userAccepted"].get<bool>());
    REQUIRE(updated->content["userFeedback"].get<std::string>() == "keep this gentler morph range");
    REQUIRE(updated->content["outcomeScore"].get<float>() > stored.content["outcomeScore"].get<float>());
    REQUIRE(updated->confidence > stored.confidence);
    REQUIRE(memory.listOutcomes("workflow-feedback-dedup-test", 10).size() == 1);

    feedback.feedbackStatus = "undo";
    feedback.userFeedback = "undo the morph move";
    const auto undone = memory.updateOutcomeFeedback(feedback);
    REQUIRE(undone.has_value());
    REQUIRE(undone->id == stored.id);
    REQUIRE(undone->content["feedbackStatus"].get<std::string>() == "undo");
    REQUIRE_FALSE(undone->content["userAccepted"].get<bool>());
    REQUIRE(undone->content["source"].get<std::string>() == "undo_feedback");
    REQUIRE(undone->content["actionId"].get<std::string>() == outcome.actionId.toStdString());
    REQUIRE(memory.listOutcomes("workflow-feedback-dedup-test", 10).size() == 1);
}

TEST_CASE("MemoryStore returns ActionOutcome evidence as advisory intent context", "[automation][memory]")
{
    ScopedAutomationStore scoped;
    more_phi::MemoryStore memory(scoped.directory);

    more_phi::ActionOutcome outcome;
    outcome.actionId = "txn-intent-context-test";
    outcome.workflowRunId = "workflow-intent-context-test";
    outcome.beforeState = nlohmann::json{{"morph_x", 0.5}};
    outcome.afterState = nlohmann::json{{"morph_x", 0.9}};
    outcome.measurements = nlohmann::json{{"more_phi_morph_x_delta", 0.4}};
    outcome.outcomeScore = 0.22f;
    outcome.source = "user_feedback";
    outcome.feedbackStatus = "too_much";
    outcome.userFeedback = "avoid aggressive morph jumps";

    const auto stored = memory.recordOutcome(outcome);
    REQUIRE(stored.id.isNotEmpty());

    const auto context = memory.intentContext(nlohmann::json{{"hostedPluginName", ""}, {"trackId", ""}}, 5);
    REQUIRE(context["backend"].get<std::string>() == "json_local_store_v1");
    REQUIRE_FALSE(context["vector_index_loaded"].get<bool>());
    REQUIRE(context["records"].is_array());

    bool foundOutcome = false;
    for (const auto& record : context["records"])
    {
        if (record["id"].get<std::string>() == stored.id.toStdString())
        {
            foundOutcome = true;
            REQUIRE(record["kind"].get<std::string>() == "action_outcome");
            REQUIRE(record["content"]["actionId"].get<std::string>() == outcome.actionId.toStdString());
            REQUIRE(record["content"]["feedbackStatus"].get<std::string>() == "too_much");
        }
    }
    REQUIRE(foundOutcome);

    more_phi::PermissionKernel permissions(scoped.directory);
    permissions.setAutonomyLevel(more_phi::AutonomyLevel::Manual);
    const auto blocked = permissions.evaluate("set_morph_position", nlohmann::json{{"x", 0.5}}, "workflow-intent-context-test");
    REQUIRE_FALSE(blocked.allowed);
    REQUIRE(blocked.risk == more_phi::RiskLevel::LowWrite);
}
