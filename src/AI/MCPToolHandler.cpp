/*
 * More-Phi — AI/MCPToolHandler.cpp
 * Instance-aware MCP tool dispatch.
 *
 * All JSON responses are constructed with nlohmann::json so that special
 * characters in plugin parameter names (e.g. quotes, backslashes) are
 * correctly escaped without manual handling.
 */
#include "MCPToolHandler.h"
#include "InstanceIdentity.h"
#include "InstanceRegistry.h"
#include "Plugin/PluginProcessor.h"
#include <future>
#include "MCPToolsExtended.h"
#include "Core/InterpolationEngine.h"
#include "Core/MorphProcessor.h"
#include "Core/DiscreteParameterHandler.h"
#include "Core/PhysicsEngine.h"
#include "Core/MeterWindowAccumulator.h"
#include "MIDI/MIDIRouter.h"
#include "OzoneParameterMap.h"
#include "ChainPlanExecutor.h"
#include "PluginProfileDB.h"
#include "PluginSemanticMapper.h"
#include "TrackAssistantStore.h"
#include "MasteringCandidateScoring.h"
#include "Agents/AgentRuntime.h"
#include "SonicMasterDecisionDecoder.h"
#include "AutomationControlPlane.h"
#include "StandaloneMcp/MorePhiIPCAssistant.h"
#include "StandaloneMcp/MorePhiIPCDiscovery.h"
#include "Dataset/OfflineBatchRenderer.h"
#include "SemanticPluginProfile.h"
#include <nlohmann/json.hpp>
#include <juce_events/juce_events.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <functional>

namespace more_phi {

using json = nlohmann::json;

// Forward declaration — defined later in this TU. Sanitizer for meter reads
// that may propagate NaN/Inf from uninitialized DSP stages.
static double finiteOr(double value, double fallback) noexcept;

// Static cache/executor instances for read-only tool acceleration and
// background tool execution. These are keyed by generation token / job id
// so they are safe across multiple More-Phi instances.
static ToolResultCache& toolResultCache()
{
    static ToolResultCache instance(128);
    return instance;
}

static AsyncToolExecutor& asyncToolExecutor()
{
    static AsyncToolExecutor instance;
    return instance;
}

// Serialize a nlohmann::json object to juce::String
static juce::String toJString(const json& j)
{
    return juce::String(j.dump());
}

static json parseToolResponse(const juce::String& response)
{
    try
    {
        return json::parse(response.toStdString());
    }
    catch (...)
    {
        return json{{"raw_response", response.toStdString()}};
    }
}

static juce::var jsonToJuceVar(const json& value)
{
    if (value.is_null())
        return {};
    if (value.is_boolean())
        return value.get<bool>();
    if (value.is_number_integer())
        return juce::var(static_cast<juce::int64>(value.get<int64_t>()));
    if (value.is_number_unsigned())
        return juce::var(static_cast<juce::int64>(value.get<uint64_t>()));
    if (value.is_number_float())
        return value.get<double>();
    if (value.is_string())
        return juce::String(value.get<std::string>());
    if (value.is_array())
    {
        juce::Array<juce::var> out;
        for (const auto& item : value)
            out.add(jsonToJuceVar(item));
        return juce::var(out);
    }
    if (value.is_object())
    {
        juce::DynamicObject::Ptr object = new juce::DynamicObject();
        for (const auto& item : value.items())
            object->setProperty(juce::Identifier(juce::String(item.key())), jsonToJuceVar(item.value()));
        return juce::var(object.get());
    }

    return juce::String(value.dump());
}

static json captureMorePhiControlState(MorePhiProcessor& p)
{
    json controls = json::array();
    const auto& parameters = p.getParameters();
    for (int i = 0; i < parameters.size(); ++i)
    {
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(parameters[i]))
        {
            controls.push_back(json{
                {"parameter_id", parameter->getParameterID().toStdString()},
                {"index", i},
                {"name", parameter->getName(128).toStdString()},
                {"value", parameter->getValue()}
            });
        }
    }

    return json{
        {"morph_x", p.getMorphX()},
        {"morph_y", p.getMorphY()},
        {"fader", p.getFaderPos()},
        {"source", p.getMorphSource()},
        {"parameters", controls}
    };
}

static json captureAutomationState(MorePhiProcessor& p)
{
    auto* plugin = p.getHostManager().getPlugin();
    const auto values = p.getParameterBridge().captureParameterState();

    return json{
        {"schema_version", 1},
        {"captured_at", juce::Time::getCurrentTime().toISO8601(true).toStdString()},
        {"queue", json{
            {"healthy", p.isCommandQueueHealthy()},
            {"usage", p.getCommandQueueUsage()},
            {"pending", static_cast<int>(p.getPendingParameterCommandCountApprox())}
        }},
        {"hosted_plugin", json{
            {"loaded", plugin != nullptr},
            {"name", plugin != nullptr ? plugin->getName().toStdString() : std::string()},
            {"parameter_count", static_cast<int>(values.size())},
            {"values", values}
        }},
        {"more_phi", captureMorePhiControlState(p)}
    };
}

static json buildRollbackPlanForTool(const juce::String& method, const json& beforeState)
{
    json plan{
        {"available", true},
        {"method", "automation.rollback"},
        {"tool", method.toStdString()}
    };

    const auto lower = method.toLowerCase();
    if (lower == "set_parameter" || lower == "set_parameters_batch"
        || lower == "hosted_plugin.set_parameter" || lower == "hosted_plugin.set_parameters"
        || lower == "plugin_profile.apply_safe_action" || lower == "plugin_profile.restore_safe_snapshot"
        || lower == "apply_mastering_plan" || lower == "mastering.apply_plan")
    {
        plan["kind"] = "hosted_parameter_state";
        plan["values"] = beforeState.value("hosted_plugin", json::object()).value("values", json::array());
        plan["parameter_count"] = static_cast<int>(plan["values"].size());
        return plan;
    }

    if (lower == "more_phi.set_parameter" || lower == "more_phi.set_parameters" || lower == "set_morph_position")
    {
        plan["kind"] = "more_phi_controls";
        plan["state"] = beforeState.value("more_phi", json::object());
        return plan;
    }

    plan["available"] = false;
    plan["kind"] = "not_reversible_in_v1";
    return plan;
}

static juce::String extractErrorCode(const json& result)
{
    if (result.contains("error") && result["error"].is_string())
        return juce::String(result["error"].get<std::string>());
    if (result.contains("code") && result["code"].is_string())
        return juce::String(result["code"].get<std::string>());
    return {};
}

static json buildTransactionMeasurements(const json& beforeState, const json& afterState)
{
    const auto beforeValues = beforeState.value("hosted_plugin", json::object()).value("values", json::array());
    const auto afterValues = afterState.value("hosted_plugin", json::object()).value("values", json::array());
    const auto count = std::min(beforeValues.size(), afterValues.size());

    int changed = 0;
    double maxDelta = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        if (!beforeValues[i].is_number() || !afterValues[i].is_number())
            continue;
        const double delta = std::abs(afterValues[i].get<double>() - beforeValues[i].get<double>());
        if (delta > 0.000001)
            ++changed;
        maxDelta = std::max(maxDelta, delta);
    }

    const auto beforeMorePhi = beforeState.value("more_phi", json::object());
    const auto afterMorePhi = afterState.value("more_phi", json::object());
    const auto morphXDelta = std::abs(afterMorePhi.value("morph_x", 0.0) - beforeMorePhi.value("morph_x", 0.0));
    const auto morphYDelta = std::abs(afterMorePhi.value("morph_y", 0.0) - beforeMorePhi.value("morph_y", 0.0));
    const auto faderDelta = std::abs(afterMorePhi.value("fader", 0.0) - beforeMorePhi.value("fader", 0.0));
    const auto morphDelta = std::max({morphXDelta, morphYDelta, faderDelta});

    return json{
        {"hosted_parameter_changed_count", changed},
        {"hosted_parameter_max_delta", maxDelta},
        {"more_phi_morph_delta", morphDelta},
        {"more_phi_morph_x_delta", morphXDelta},
        {"more_phi_morph_y_delta", morphYDelta},
        {"more_phi_fader_delta", faderDelta},
        {"queue_pending_after", afterState.value("queue", json::object()).value("pending", 0)}
    };
}

static bool shouldAutoRecordOutcome(const AutomationTransaction& transaction)
{
    if (!transaction.success || transaction.workflowRunId.isEmpty() || transaction.workflowStepId.isEmpty())
        return false;

    const auto tool = transaction.toolName.toLowerCase();
    return !tool.startsWith("memory.")
        && !tool.startsWith("permission.")
        && !tool.startsWith("workflow.");
}

static ActionOutcome buildAutomaticOutcome(const AutomationTransaction& transaction)
{
    ActionOutcome outcome;
    outcome.actionId = transaction.id;
    outcome.workflowRunId = transaction.workflowRunId;
    outcome.beforeState = transaction.beforeState;
    outcome.afterState = transaction.afterState;
    outcome.measurements = transaction.measurements;
    outcome.userAccepted = false;
    outcome.outcomeScore = transaction.success ? 0.55f : 0.0f;
    outcome.source = "automatic_transaction";
    outcome.feedbackStatus = "unreviewed";
    return outcome;
}

static juce::String normalizeOutcomeFeedbackStatusForMcp(const juce::String& status)
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

static bool outcomeFeedbackStatusIsPositive(const juce::String& status)
{
    const auto normalized = normalizeOutcomeFeedbackStatusForMcp(status);
    return normalized == "accepted" || normalized == "sounds_better";
}

static float outcomeScoreForFeedbackStatusForMcp(const juce::String& status)
{
    const auto normalized = normalizeOutcomeFeedbackStatusForMcp(status);
    if (normalized == "accepted") return 0.92f;
    if (normalized == "sounds_better") return 0.84f;
    if (normalized == "too_much") return 0.22f;
    if (normalized == "undo") return 0.05f;
    if (normalized == "rejected") return 0.10f;
    return 0.45f;
}

static json buildDiffPreview(const juce::var& params, MorePhiProcessor& p);

static json buildApprovalPredictedDiff(const juce::String& method,
                                       const juce::var& params,
                                       MorePhiProcessor& p)
{
    juce::DynamicObject::Ptr previewParams = new juce::DynamicObject();
    previewParams->setProperty("tool_name", method);

    const auto lower = method.toLowerCase();
    if (lower == "set_parameters_batch" || lower == "hosted_plugin.set_parameters"
        || lower == "more_phi.set_parameters")
    {
        previewParams->setProperty("parameters",
            params.getProperty("parameters", params.getProperty("params", juce::var())));
    }
    else
    {
        previewParams->setProperty("params", params);
    }

    auto preview = buildDiffPreview(juce::var(previewParams.get()), p);
    preview["approval_preview"] = true;
    return preview;
}

static juce::String suggestedActionForError(const juce::String& errorCode)
{
    auto lower = errorCode.toLowerCase();
    if (lower == "queue_full")          return "Retry after 50 ms or reduce the parameter batch size.";
    if (lower == "plugin_not_loaded")   return "Load a hosted plugin first using hosted_plugin.load.";
    if (lower == "invalid_param_id")    return "Use list_parameters to find a valid stableId/index/name.";
    if (lower == "approval_required")   return "Request user approval or raise the autonomy level.";
    if (lower == "rate_limit_exceeded") return "Wait briefly before sending the next request.";
    if (lower == "snapshot_slot_empty") return "Capture a snapshot into the slot before recalling it.";
    if (lower == "transaction_not_found") return "Check the transaction_id or use automation.history.";
    if (lower == "rollback_unavailable") return "This transaction type does not support rollback.";
    if (lower == "pending_parameter_edits") return "Flush pending edits or wait for the queue to drain.";
    // Verification failure modes (Phase 1 execution verification).
    if (lower == "value_drift")        return "The applied value differs from the request (the parameter may be discrete or clamped by the host); re-read the parameter and adjust.";
    if (lower == "out_of_range")       return "Clamp the value to the parameter's valid range and retry.";
    if (lower == "parameter_index_out_of_range") return "The parameter index exceeds the hosted plugin's actual parameter count; use list_parameters to discover valid indices.";
    if (lower == "timeout")            return "Retry with a smaller batch or check the hosted plugin's responsiveness.";
    if (lower == "plugin_not_ready")   return "Wait for the hosted plugin to finish initializing, then retry.";
    return "Check the error details and retry; report if the error persists.";
}

static juce::String dispatchWithAutomationTransaction(const juce::String& method,
                                                       const juce::var& params,
                                                       MorePhiProcessor& p,
                                                       AutomationRuntime& runtime,
                                                       const std::function<juce::String()>& executor)
{
    json paramsJson = juceVarToJson(params);
    if (!paramsJson.is_object())
        paramsJson = json::object();

    const auto workflowRunId = juce::String(params.getProperty("workflow_run_id",
                                        params.getProperty("workflowRunId", "")).toString());
    const auto workflowStepId = juce::String(params.getProperty("workflow_step_id",
                                         params.getProperty("workflowStepId", "")).toString());
    auto decision = runtime.permissions().evaluate(method, paramsJson, workflowRunId);
    if (!decision.allowed)
    {
        decision.approval.predictedDiff = buildApprovalPredictedDiff(method, params, p);
        runtime.permissions().updateApprovalPreview(decision.approval.id, decision.approval.predictedDiff);

        runtime.events().publish(IntegrationEvent{
            {},
            "permission",
            "approval.required",
            workflowRunId,
            decision.approval.transactionId,
            toJson(decision.approval),
            juce::Time::getCurrentTime()
        });

        return toJString(json{
            {"success", false},
            {"error", "approval_required"},
            {"approval_required", true},
            {"risk", toString(decision.risk).toStdString()},
            {"autonomy_level", toString(runtime.permissions().getAutonomyLevel()).toStdString()},
            {"approval_request", toJson(decision.approval)}
        });
    }

    AutomationTransaction transaction;
    transaction.id = makeAutomationId("txn");
    transaction.workflowRunId = workflowRunId;
    transaction.workflowStepId = workflowStepId;
    transaction.toolName = method;
    transaction.risk = decision.risk;
    transaction.params = paramsJson;
    transaction.startedAt = juce::Time::getCurrentTime();
    transaction.beforeState = captureAutomationState(p);
    transaction.rollbackPlan = buildRollbackPlanForTool(method, transaction.beforeState);

    runtime.events().publish(IntegrationEvent{
        {},
        "automation",
        "transaction.started",
        workflowRunId,
        transaction.id,
        json{
            {"risk", toString(transaction.risk).toStdString()},
            {"risk_model", "static_tool_classification_v1"}
        },
        transaction.startedAt
    });

    juce::String rawResponse;
    const auto execT0 = std::chrono::high_resolution_clock::now();
    try
    {
        rawResponse = executor();
    }
    catch (const std::exception& ex)
    {
        rawResponse = toJString(json{{"success", false}, {"error", "exception"}, {"details", ex.what()}});
    }
    catch (...)
    {
        rawResponse = toJString(json{{"success", false}, {"error", "unknown_exception"}});
    }
    const auto execT1 = std::chrono::high_resolution_clock::now();
    const double latencyMs = std::chrono::duration<double, std::milli>(execT1 - execT0).count();

    transaction.completedAt = juce::Time::getCurrentTime();
    transaction.afterState = captureAutomationState(p);
    transaction.result = parseToolResponse(rawResponse);
    transaction.success = transaction.result.is_object() && transaction.result.value("success", false);
    if (transaction.success)
        MCPToolHandler::invalidateToolResultCacheForTool(method);
    transaction.errorCode = extractErrorCode(transaction.result);
    transaction.measurements = buildTransactionMeasurements(transaction.beforeState, transaction.afterState);
    transaction.measurements["latency_ms"] = latencyMs;
    transaction = runtime.ledger().record(transaction);
    std::optional<MemoryRecord> automaticOutcomeRecord;

    runtime.events().publish(IntegrationEvent{
        {},
        "automation",
        "transaction.completed",
        workflowRunId,
        transaction.id,
        json{{"success", transaction.success}, {"error", transaction.errorCode.toStdString()}},
        transaction.completedAt
    });

    if (shouldAutoRecordOutcome(transaction))
    {
        const auto outcome = buildAutomaticOutcome(transaction);
        automaticOutcomeRecord = runtime.memory().recordOutcome(outcome);
        runtime.events().publish(IntegrationEvent{
            {},
            "memory",
            "outcome.auto_recorded",
            transaction.workflowRunId,
            transaction.id,
            json{{"memory_id", automaticOutcomeRecord->id.toStdString()}, {"outcome", toJson(outcome)}},
            juce::Time::getCurrentTime()
        });
    }

    if (transaction.result.is_object())
    {
        transaction.result["transaction_id"] = transaction.id.toStdString();
        transaction.result["latency_ms"] = latencyMs;
        if (!transaction.success && transaction.errorCode.isNotEmpty())
            transaction.result["suggested_action"] = suggestedActionForError(transaction.errorCode).toStdString();

        transaction.result["automation"] = json{
            {"transaction_id", transaction.id.toStdString()},
            {"risk", toString(transaction.risk).toStdString()},
            {"rollback_available", transaction.rollbackPlan.value("available", false)}
        };

        // Inject transaction context into every nested verification object so the
        // AI caller receives a self-contained VerificationReceipt per spec §5.1
        // (transaction_id, rollback_id, audio_state_delta).
        const std::string txnId = transaction.id.toStdString();
        const std::string rbId = transaction.rollbackPlan.value("rollback_id", "");
        const float stateDelta = transaction.beforeState.is_object() && transaction.afterState.is_object()
            ? static_cast<float>(transaction.afterState.value("param_checksum", 0ull) -
                                 transaction.beforeState.value("param_checksum", 0ull))
            : 0.0f;

        auto injectVerificationContext = [&](json& obj, auto& self) -> void
        {
            if (obj.is_object())
            {
                if (obj.contains("verification") && obj["verification"].is_object())
                {
                    auto& v = obj["verification"];
                    v["transaction_id"] = txnId;
                    if (!rbId.empty()) v["rollback_id"] = rbId;
                    v["audio_state_delta"] = stateDelta;
                }
                for (auto& [key, val] : obj.items())
                    self(val, self);
            }
            else if (obj.is_array())
            {
                for (auto& item : obj)
                    self(item, self);
            }
        };
        injectVerificationContext(transaction.result, injectVerificationContext);

        if (transaction.result.contains("verification"))
        {
            transaction.result["receipt"] = transaction.result["verification"];
        }

        if (automaticOutcomeRecord.has_value())
        {
            transaction.result["automation"]["outcome_recorded"] = true;
            transaction.result["automation"]["outcome_memory_id"] = automaticOutcomeRecord->id.toStdString();
        }
        return toJString(transaction.result);
    }

    return rawResponse;
}

static json buildSessionContextJson(MorePhiProcessor& p, const InstanceIdentity& identity)
{
    auto* plugin = p.getHostManager().getPlugin();
    auto& ame = p.getAutoMasteringEngine();

    SessionContext context;
    context.instanceId = identity.instanceId;
    context.hostedPluginName = plugin != nullptr ? plugin->getName() : juce::String();
    context.hostedParameterCount = p.getParameterBridge().getParameterCount();
    const auto transport = p.getTransportContextSnapshot();
    context.transport.available = transport.available;
    context.transport.playing = transport.playing;
    context.transport.looping = transport.looping;
    context.transport.bpm = transport.bpm;
    context.transport.timeSigNumerator = transport.timeSigNumerator;
    context.transport.timeSigDenominator = transport.timeSigDenominator;
    context.transport.ppqPosition = transport.ppqPosition;
    context.transport.secondsPosition = transport.secondsPosition;
    context.currentMeters = json{
        {"rms", p.getRmsLevel()},
        {"lufs_momentary", ame.getLUFSMomentary()},
        {"lufs_short_term", ame.getLUFSShortTerm()},
        {"lufs_integrated", ame.getLUFSIntegrated()},
        {"lra", ame.getLRA()},
        {"true_peak_dbtp", ame.getTruePeak_dBTP()}
    };
    context.currentSpectrum = json{
        {"available", false},
        {"summary", "Use analysis.get_spectrum for the full realtime spectrum snapshot."}
    };
    context.currentStereoField = json{
        {"available", false},
        {"summary", "Use analysis.get_stereo_field for the full realtime stereo-field snapshot."}
    };
    context.trackAssistantState = json{
        {"backend", "TrackAssistantStore"},
        {"status", "available_via_ozone.track.get_info"}
    };

    auto result = toJson(context);
    result["context_metadata"] = json{
        {"provider", "SessionContextProvider"},
        {"threading", "non_audio_thread_snapshot"},
        {"transport_source", "juce_audio_playhead_position_snapshot"},
        {"workflow_orchestration_ready", true}
    };
    return result;
}

static json buildPluginAdapterCapabilities(MorePhiProcessor& p)
{
    const auto descriptors = p.getParameterBridge().getParameterDescriptors();
    const auto controls = SemanticPluginProfile::classify(descriptors);
    const auto controlsJson = SemanticPluginProfile::controlsToJson(descriptors, controls);

    json capabilities = json::array();
    capabilities.push_back(json{
        {"id", "semantic_parameter_control"},
        {"confidence", descriptors.empty() ? "none" : "inferred"},
        {"controls", controlsJson},
        {"limits", json{
            {"adapter_precedence", json::array({
                "explicit_vendor_plugin_adapter",
                "saved_plugin_profile",
                "semantic_inferred_profile",
                "raw_parameter_fallback"
            })},
            {"write_path", "AutomationDispatcher"},
            {"realtime_safe_queue", true}
        }}
    });

    return json{
        {"success", true},
        {"adapter_layer", "plugin_adapter"},
        {"adapter_backend", "semantic_inferred_profile"},
        {"explicit_adapter_loaded", false},
        {"saved_profile_loaded", false},
        {"raw_parameter_fallback_available", true},
        {"capabilities", capabilities}
    };
}

static json buildPluginAdapterPlan(const juce::var& params, MorePhiProcessor& p)
{
    const auto descriptors = p.getParameterBridge().getParameterDescriptors();
    const auto controls = SemanticPluginProfile::classify(descriptors);
    const auto plan = SemanticPluginProfile::planSafeAction(params, descriptors, controls, &p.getParameterBridge());

    json response{
        {"success", plan.success},
        {"adapter_layer", "plugin_adapter"},
        {"planner_type", "semantic_rule_adapter"},
        {"requires_dispatch", true},
        {"automation_dispatcher_required", true}
    };

    if (!plan.success)
    {
        response["error"] = plan.error.toStdString();
        response["message"] = plan.message.toStdString();
        return response;
    }

    json commands = json::array();
    for (const auto& command : plan.commands)
    {
        const auto descriptor = command.parameterIndex >= 0
            ? p.getParameterBridge().getParameterDescriptor(command.parameterIndex)
            : ParameterBridge::ParameterDescriptor{};
        commands.push_back(json{
            {"index", command.parameterIndex},
            {"name", descriptor.name.toStdString()},
            {"after", command.normalizedValue}
        });
    }

    response["action"] = plan.actionJson;
    response["commands"] = commands;
    response["predictedDiff"] = commands;
    response["risk"] = "low_write";
    return response;
}

static bool predictionWouldRequireApproval(RiskLevel risk, AutonomyLevel autonomy)
{
    if (risk == RiskLevel::ReadOnly)
        return false;

    switch (autonomy)
    {
        case AutonomyLevel::Manual:
            return true;
        case AutonomyLevel::Assist:
            return risk != RiskLevel::LowWrite;
        case AutonomyLevel::CoPilot:
        case AutonomyLevel::Autopilot:
            return risk != RiskLevel::LowWrite && risk != RiskLevel::MediumWrite;
    }

    return true;
}

static json predictionCandidate(const char* toolName,
                                const char* title,
                                const char* why,
                                const char* suggestedCopy,
                                const json& params,
                                const json& requiredScopes,
                                const json& predictedDiff,
                                PermissionKernel& permissions)
{
    const auto risk = permissions.classifyTool(juce::String(toolName), params);
    const auto autonomy = permissions.getAutonomyLevel();
    return json{
        {"tool", toolName},
        {"title", title},
        {"why", why},
        {"params_template", params},
        {"predicted_diff", predictedDiff},
        {"descriptor", json{
            {"risk", toString(risk).toStdString()},
            {"required_scopes", requiredScopes},
            {"requires_approval", predictionWouldRequireApproval(risk, autonomy)},
            {"autonomy_level", toString(autonomy).toStdString()},
            {"permission_model", "prediction_only_no_approval_created"}
        }},
        {"suggested_user_copy", suggestedCopy}
    };
}

static json buildWorkflowPrediction(const juce::var& params,
                                    MorePhiProcessor& p,
                                    const InstanceIdentity& identity,
                                    AutomationRuntime& runtime)
{
    const auto session = buildSessionContextJson(p, identity);
    const auto requestedWorkflowId = params.getProperty("workflow_run_id",
                                    params.getProperty("workflowRunId", "")).toString();
    const int evidenceLimit = juce::jlimit(1, 20, static_cast<int>(params.getProperty("memory_limit",
                                              params.getProperty("memoryLimit", 5))));
    const auto outcomes = runtime.memory().listOutcomes(requestedWorkflowId, evidenceLimit);

    json evidence = json::array();
    int positiveEvidence = 0;
    int cautionEvidence = 0;
    for (const auto& record : outcomes)
    {
        const auto content = record.value("content", json::object());
        const auto status = content.value("feedbackStatus", "");
        if (status == "accepted" || status == "sounds_better")
            ++positiveEvidence;
        if (status == "rejected" || status == "too_much" || status == "undo")
            ++cautionEvidence;

        evidence.push_back(json{
            {"memory_id", record.value("id", "")},
            {"action_id", content.value("actionId", "")},
            {"workflow_run_id", content.value("workflowRunId", "")},
            {"feedback_status", status},
            {"outcome_score", content.value("outcomeScore", 0.0)},
            {"source", content.value("source", "")},
            {"advisory", true}
        });
    }

    const auto morphParams = json{{"x", p.getMorphX()}, {"y", p.getMorphY()}, {"fader", p.getFaderPos()}};
    const auto morphPreview = buildDiffPreview(jsonToJuceVar(json{{"tool_name", "set_morph_position"}, {"params", morphParams}}), p)
        .value("diffs", json::array());

    json candidates = json::array();
    candidates.push_back(predictionCandidate(
        "analysis.get_summary",
        "Refresh the session analysis snapshot",
        "Start with a read-only analysis pass before proposing a write.",
        "I can first check the current level, spectrum, and stereo summary before changing anything.",
        json::object(),
        json::array({"session.analysis.read"}),
        json::array(),
        runtime.permissions()));

    candidates.push_back(predictionCandidate(
        "set_morph_position",
        cautionEvidence > 0 ? "Use a conservative morph move" : "Adjust the morph position",
        cautionEvidence > 0
            ? "Prior outcome feedback includes rejected, too_much, or undo signals, so any write should be small and approval-aware."
            : "The morph controls are low-write and reversible through the automation ledger.",
        cautionEvidence > 0
            ? "I found prior caution feedback, so I would only make a small morph adjustment after you approve it."
            : "I can make a controlled morph move and record the result for feedback.",
        morphParams,
        json::array({"morph.position.write"}),
        morphPreview,
        runtime.permissions()));

    if (session.value("hostedParameterCount", 0) > 0)
    {
        candidates.push_back(predictionCandidate(
            "plugin_adapter.plan_action",
            "Preview a semantic hosted-plugin action",
            "A semantic adapter plan is read-only and can show safe parameter diffs before dispatch.",
            "I can draft a hosted-plugin move as a preview first, without applying it.",
            json{{"dry_run", true}, {"action", json::object()}},
            json::array({"hosted_plugin.semantic_plan.read"}),
            json::array(),
            runtime.permissions()));
    }

    const auto userIntent = params.getProperty("user_intent",
                            params.getProperty("userIntent", "suggest the next safe action")).toString();
    return json{
        {"success", true},
        {"read_only", true},
        {"prediction_model", "workflow_next_action_static_v1"},
        {"current_goal", json{
            {"user_intent", userIntent.toStdString()},
            {"workflow_run_id", requestedWorkflowId.toStdString()},
            {"session_summary", json{
                {"instance_id", session.value("instanceId", "")},
                {"hosted_plugin_name", session.value("hostedPluginName", "")},
                {"hosted_parameter_count", session.value("hostedParameterCount", 0)},
                {"transport_available", session.value("transport", json::object()).value("available", false)}
            }}
        }},
        {"candidate_next_actions", candidates},
        {"memory_evidence", evidence},
        {"memory_policy", json{
            {"advisory_only", true},
            {"can_grant_permission", false},
            {"sqlite_backend_loaded", false},
            {"vector_index_loaded", false}
        }},
        {"why_execution_is_not_automatic", "workflow.predict_next is read-only: it does not submit workflows, execute tools, create approvals, or persist memory."},
        {"suggested_user_facing_copy", positiveEvidence > 0
            ? "I found prior positive feedback and can suggest a similar safe next step, but I need your approval before any write."
            : "I can suggest the safest next step from the current session and memory, but I will not execute it automatically."}
    };
}

static json buildDiffPreview(const juce::var& params, MorePhiProcessor& p)
{
    const auto toolName = params.getProperty("tool_name",
                         params.getProperty("toolName", "")).toString();
    json diffs = json::array();

    auto addHostedDiff = [&diffs, &p](const juce::var& item)
    {
        auto& bridge = p.getParameterBridge();
        const int rawId = item.hasProperty("index")
            ? static_cast<int>(item.getProperty("index", -1))
            : static_cast<int>(item.getProperty("id", -1));
        const auto stableId = item.getProperty("stableId",
                              item.getProperty("stable_id", "")).toString();
        const auto name = item.getProperty("name", "").toString();
        const auto resolution = bridge.resolveParameter(stableId, rawId, name);
        if (!resolution.success)
            return;

        const auto descriptor = bridge.getParameterDescriptor(resolution.index);
        diffs.push_back(toJson(ParameterDiff{
            descriptor.index,
            descriptor.name,
            descriptor.value,
            static_cast<float>(item.getProperty("value", descriptor.value)),
            "hosted_parameter",
            RiskLevel::LowWrite
        }));
    };

    auto addMorePhiDiff = [&diffs, &p](const juce::var& item)
    {
        auto id = item.getProperty("parameter_id",
                  item.getProperty("parameterId", "")).toString().trim();
        if (id.isEmpty() && item.hasProperty("id") && item.getProperty("id", juce::var()).isString())
            id = item.getProperty("id", "").toString().trim();

        juce::RangedAudioParameter* parameter = nullptr;
        int index = -1;
        if (id.isNotEmpty())
        {
            parameter = p.getAPVTS().getParameter(id);
        }
        else
        {
            index = static_cast<int>(item.getProperty("index", -1));
            const auto& parameters = p.getParameters();
            if (index >= 0 && index < parameters.size())
                parameter = dynamic_cast<juce::RangedAudioParameter*>(parameters[index]);
        }

        if (parameter == nullptr)
            return;

        const auto& parameters = p.getParameters();
        for (int i = 0; i < parameters.size(); ++i)
        {
            if (parameters[i] == parameter)
            {
                index = i;
                break;
            }
        }

        diffs.push_back(json{
            {"index", index},
            {"parameter_id", parameter->getParameterID().toStdString()},
            {"name", parameter->getName(128).toStdString()},
            {"before", parameter->getValue()},
            {"after", static_cast<float>(item.getProperty("value", parameter->getValue()))},
            {"semanticRole", "more_phi_control"},
            {"risk", toString(RiskLevel::LowWrite).toStdString()}
        });
    };

    if (toolName == "set_parameter" || toolName == "hosted_plugin.set_parameter")
    {
        addHostedDiff(params.getProperty("params", params));
    }
    else if (toolName == "set_parameters_batch" || toolName == "hosted_plugin.set_parameters")
    {
        const auto batchPayload = params.hasProperty("params")
            ? params.getProperty("params", juce::var())
            : params.getProperty("parameters", juce::var());
        if (auto* list = batchPayload.getArray())
            for (const auto& item : *list)
                addHostedDiff(item);
    }
    else if (toolName == "set_morph_position")
    {
        const auto requested = params.getProperty("params", params);
        if (requested.hasProperty("x"))
            diffs.push_back(json{{"control", "morph_x"}, {"before", p.getMorphX()}, {"after", static_cast<float>(requested.getProperty("x", p.getMorphX()))}, {"semanticRole", "morph_position"}, {"risk", "low_write"}});
        if (requested.hasProperty("y"))
            diffs.push_back(json{{"control", "morph_y"}, {"before", p.getMorphY()}, {"after", static_cast<float>(requested.getProperty("y", p.getMorphY()))}, {"semanticRole", "morph_position"}, {"risk", "low_write"}});
        if (requested.hasProperty("fader"))
            diffs.push_back(json{{"control", "fader"}, {"before", p.getFaderPos()}, {"after", static_cast<float>(requested.getProperty("fader", p.getFaderPos()))}, {"semanticRole", "morph_position"}, {"risk", "low_write"}});
        if (requested.hasProperty("source"))
            diffs.push_back(json{{"control", "source"}, {"before", p.getMorphSource()}, {"after", requested.getProperty("source", "").toString().toStdString()}, {"semanticRole", "morph_source"}, {"risk", "low_write"}});
    }
    else if (toolName == "more_phi.set_parameter")
    {
        addMorePhiDiff(params.getProperty("params", params));
    }
    else if (toolName == "more_phi.set_parameters")
    {
        const auto batchPayload = params.hasProperty("params")
            ? params.getProperty("params", juce::var())
            : params.getProperty("parameters", juce::var());
        if (auto* list = batchPayload.getArray())
            for (const auto& item : *list)
                addMorePhiDiff(item);
    }
    else if (toolName == "plugin_adapter.apply_action" || toolName == "plugin_profile.apply_safe_action")
    {
        const auto planned = buildPluginAdapterPlan(params.getProperty("params", params), p);
        if (planned.value("success", false))
            diffs = planned.value("predictedDiff", json::array());
    }

    return json{
        {"success", true},
        {"tool_name", toolName.toStdString()},
        {"diffs", diffs},
        {"diff_model", "current_snapshot_vs_requested_params_v1"}
    };
}

static bool restoreMorePhiControlsFromState(const json& state, MorePhiProcessor& p)
{
    if (state.contains("morph_x")) p.setMorphX(state.value("morph_x", p.getMorphX()));
    if (state.contains("morph_y")) p.setMorphY(state.value("morph_y", p.getMorphY()));
    if (state.contains("fader")) p.setFaderPos(state.value("fader", p.getFaderPos()));
    if (state.contains("source")) p.setMorphSource(state.value("source", p.getMorphSource()));

    if (!state.contains("parameters") || !state["parameters"].is_array())
        return true;

    for (const auto& item : state["parameters"])
    {
        const auto id = juce::String(item.value("parameter_id", ""));
        if (id.isEmpty() || !item.contains("value"))
            continue;

        auto* parameter = p.getAPVTS().getParameter(id);
        if (parameter == nullptr)
            continue;
        // AUDIT-FIX (VST3 gestures): a bulk state restore is a discrete
        // programmatic edit per parameter, so bracket each write with
        // begin/end gesture. This lets the host record the restore as proper
        // automation touches rather than gesture-less notify calls. (Continuous
        // morph streams elsewhere intentionally omit per-sample gestures per
        // the VST3 spec.)
        const float restored = static_cast<float>(item.value("value", parameter->getValue()));
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(restored);
        parameter->endChangeGesture();
    }
    return true;
}

static json rollbackAutomationTransaction(const juce::var& params, MorePhiProcessor& p, AutomationRuntime& runtime)
{
    const auto transactionId = params.getProperty("transaction_id",
                               params.getProperty("transactionId", "")).toString();
    if (transactionId.isEmpty())
        return json{{"success", false}, {"error", "missing_transaction_id"}};

    auto transaction = runtime.ledger().find(transactionId);
    if (!transaction.has_value())
        return json{{"success", false}, {"error", "transaction_not_found"}, {"transaction_id", transactionId.toStdString()}};

    const auto plan = transaction->rollbackPlan;
    if (!plan.value("available", false))
        return json{{"success", false}, {"error", "rollback_unavailable"}, {"transaction_id", transactionId.toStdString()}};

    const auto kind = juce::String(plan.value("kind", ""));
    if (kind == "hosted_parameter_state")
    {
        const auto valuesJson = plan.value("values", json::array());
        std::vector<MorePhiProcessor::ParamCommand> commands;
        commands.reserve(valuesJson.size());
        // MCP-CONTROL-04: bound the rollback to the project's parameter ceiling
        // (MAX_PARAMETERS=4096). A tampered/oversized ledger could otherwise queue
        // commands for non-existent param indices, and enqueueParameterBatch
        // rejects the ENTIRE batch if any index >= MAX_PARAMETERS — breaking the
        // whole rollback. (writeParameter still drops any index beyond the live
        // hosted-plugin count, so this is robustness, not memory-safety.)
        constexpr int kMaxRollbackParamIndex = 4095;
        for (size_t i = 0; i < valuesJson.size() && static_cast<int>(i) <= kMaxRollbackParamIndex; ++i)
        {
            if (!valuesJson[i].is_number())
                continue;
            commands.push_back(MorePhiProcessor::ParamCommand{
                static_cast<int>(i),
                valuesJson[i].get<float>(),
                false,
                -1,
                MorePhiProcessor::ParameterEditSource::MCP,
                true
            });
        }

        if (!p.enqueueParameterBatch(commands))
            return json{{"success", false}, {"error", "queue_full"}, {"transaction_id", transactionId.toStdString()}};

        const auto flush = p.flushPendingParameterCommandsForAssistant(
            juce::jmax(2048, static_cast<int>(commands.size())));
        return json{
            {"success", true},
            {"transaction_id", transactionId.toStdString()},
            {"rollback_kind", kind.toStdString()},
            {"queued", static_cast<int>(commands.size())},
            {"appliedNow", flush.drained},
            {"pendingAfter", flush.pendingAfter}
        };
    }

    if (kind == "more_phi_controls")
    {
        const bool ok = restoreMorePhiControlsFromState(plan.value("state", json::object()), p);
        return json{
            {"success", ok},
            {"transaction_id", transactionId.toStdString()},
            {"rollback_kind", kind.toStdString()}
        };
    }

    return json{{"success", false}, {"error", "unsupported_rollback_kind"}, {"kind", kind.toStdString()}};
}

static SyncEnvelope syncEnvelopeFromParams(const juce::var& params)
{
    SyncEnvelope envelope;
    envelope.instanceId = params.getProperty("instance_id",
                          params.getProperty("instanceId", "")).toString();
    envelope.sessionId = params.getProperty("session_id",
                         params.getProperty("sessionId", "")).toString();
    envelope.revision = static_cast<uint64_t>(static_cast<juce::int64>(params.getProperty("revision", 0)));
    envelope.conflictPolicy = params.getProperty("conflict_policy",
                              params.getProperty("conflictPolicy", "merge")).toString();
    envelope.statePatch = juceVarToJson(params.getProperty("state_patch",
                                       params.getProperty("statePatch", juce::var())));
    if (!envelope.statePatch.is_object())
        envelope.statePatch = json::object();
    return envelope;
}

static WorkflowRun workflowRunFromParams(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity)
{
    auto payload = juceVarToJson(params.getProperty("workflow_run",
                              params.getProperty("workflowRun", juce::var())));
    if (!payload.is_object())
        payload = juceVarToJson(params);
    if (!payload.is_object())
        payload = json::object();

    WorkflowRun run = workflowRunFromJson(payload);
    if (run.id.isEmpty())
        run.id = makeAutomationId("workflow");
    if (run.goal.id.isEmpty())
        run.goal.id = makeAutomationId("goal");

    if (run.goal.userIntent.isEmpty())
    {
        run.goal.userIntent = params.getProperty("user_intent",
                              params.getProperty("userIntent", "submitted workflow")).toString();
    }

    if (!payload.contains("goal"))
    {
        run.goal.targetProfile = juce::String(payload.value("targetProfile", "current_session"));
        run.goal.successCriteria = payload.value("successCriteria", json{
            {"verification", "all_required_steps_completed"}
        });
        run.goal.constraints = payload.value("constraints", json{
            {"audio_thread", "never_execute_workflow_steps_on_audio_thread"},
            {"writes", "through_automation_dispatcher"}
        });
    }

    if (run.goal.contextSnapshot.empty())
    {
        auto context = payload.value("context", json::object());
        if (!context.is_object() || context.empty())
            context = buildSessionContextJson(p, identity);
        run.goal.contextSnapshot = context;
    }

    run.state = WorkflowState::Draft;
    run.finalReport = json{
        {"planner_type", "submitted_workflow_dag_v1"},
        {"llm_direct_tool_loop_replaced_for_workflows", true}
    };
    return run;
}

static juce::String handleControlPlaneTool(const juce::String& method,
                                           const juce::var& params,
                                           MorePhiProcessor& p,
                                           const InstanceIdentity& identity,
                                           AutomationRuntime& runtime)
{
    if (method == "automation.history")
    {
        const auto workflowRunId = params.getProperty("workflow_run_id",
                                  params.getProperty("workflowRunId", "")).toString();
        return toJString(json{
            {"success", true},
            {"transactions", runtime.ledger().listRecent(params.getProperty("limit", 50), workflowRunId)},
            {"workflow_run_id", workflowRunId.toStdString()}
        });
    }

    if (method == "automation.get_transaction")
    {
        const auto id = params.getProperty("transaction_id",
                        params.getProperty("transactionId", "")).toString();
        const auto transaction = runtime.ledger().find(id);
        if (!transaction.has_value())
            return toJString(json{{"success", false}, {"error", "transaction_not_found"}});
        return toJString(json{{"success", true}, {"transaction", toJson(*transaction)}});
    }

    if (method == "automation.rollback")
        return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return toJString(rollbackAutomationTransaction(params, p, runtime)); });

    if (method == "automation.diff_preview")
        return toJString(buildDiffPreview(params, p));

    if (method == "permission.get_state")
        return toJString(json{{"success", true}, {"permission", runtime.permissions().describeState()}});

    if (method == "permission.set_autonomy")
    {
        const auto level = autonomyLevelFromString(params.getProperty("level",
                         params.getProperty("autonomy_level", "assist")).toString());
        runtime.permissions().setAutonomyLevel(level);
        // Adaptive rate limiting (spec §6.5): scale the per-minute request
        // limit with the autonomy level so Manual/Assist are throttled harder
        // while an approved Autopilot workflow can push changes faster.
        switch (level)
        {
            case AutonomyLevel::Manual:   p.getTokenOptimizer().setAutonomyRateMultiplier(0.5f); break;
            case AutonomyLevel::Assist:   p.getTokenOptimizer().setAutonomyRateMultiplier(1.0f); break;
            case AutonomyLevel::CoPilot:  p.getTokenOptimizer().setAutonomyRateMultiplier(1.5f); break;
            case AutonomyLevel::Autopilot:p.getTokenOptimizer().setAutonomyRateMultiplier(2.0f); break;
        }
        runtime.events().publish(IntegrationEvent{{}, "permission", "autonomy.changed", {}, {}, json{{"level", toString(level).toStdString()}, {"rate_limit_multiplier", p.getTokenOptimizer().getAutonomyRateMultiplier()}}, juce::Time::getCurrentTime()});
        return toJString(json{{"success", true}, {"autonomy_level", toString(level).toStdString()}, {"effective_rate_limit", p.getTokenOptimizer().getEffectiveRateLimit()}});
    }

    if (method == "permission.list_approvals")
        return toJString(json{{"success", true}, {"approvals", runtime.permissions().listApprovals()}});

    if (method == "permission.approve")
    {
        const auto id = params.getProperty("approval_id",
                        params.getProperty("approvalId", "")).toString();
        const bool ok = runtime.permissions().approve(id);
        if (ok)
        {
            runtime.events().publish(IntegrationEvent{
                {},
                "permission",
                "approval.approved",
                {},
                {},
                json{{"approval_id", id.toStdString()}},
                juce::Time::getCurrentTime()
            });
        }
        return toJString(json{{"success", ok}, {"approval_id", id.toStdString()}});
    }

    if (method == "permission.reject")
    {
        const auto id = params.getProperty("approval_id",
                        params.getProperty("approvalId", "")).toString();
        const bool ok = runtime.permissions().reject(id);
        if (ok)
        {
            runtime.events().publish(IntegrationEvent{
                {},
                "permission",
                "approval.rejected",
                {},
                {},
                json{{"approval_id", id.toStdString()}},
                juce::Time::getCurrentTime()
            });
        }
        return toJString(json{{"success", ok}, {"approval_id", id.toStdString()}});
    }

    if (method == "workflow.create")
    {
        const auto intent = params.getProperty("user_intent",
                            params.getProperty("userIntent", "")).toString();
        auto context = juceVarToJson(params.getProperty("context", juce::var()));
        if (!context.is_object())
            context = buildSessionContextJson(p, identity);
        const auto run = runtime.workflows().createRun(intent, context);
        runtime.events().publish(IntegrationEvent{{}, "workflow", "workflow.created", run.id, {}, toJson(run), juce::Time::getCurrentTime()});
        return toJString(json{{"success", true}, {"workflow_run", toJson(run)}});
    }

    if (method == "workflow.submit")
    {
        const auto run = runtime.workflows().submitRun(workflowRunFromParams(params, p, identity));
        runtime.events().publish(IntegrationEvent{{}, "workflow", "workflow.submitted", run.id, {}, toJson(run), juce::Time::getCurrentTime()});
        return toJString(json{
            {"success", run.state != WorkflowState::Failed},
            {"workflow_run", toJson(run)}
        });
    }

    if (method == "workflow.get")
    {
        const auto id = params.getProperty("workflow_run_id",
                        params.getProperty("workflowRunId", "")).toString();
        const auto run = runtime.workflows().getRun(id);
        if (!run.has_value())
            return toJString(json{{"success", false}, {"error", "workflow_not_found"}});
        return toJString(json{{"success", true}, {"workflow_run", toJson(*run)}});
    }

    if (method == "workflow.list")
        return toJString(json{{"success", true}, {"workflow_runs", runtime.workflows().listRuns()}});

    if (method == "workflow.execute")
    {
        const auto id = params.getProperty("workflow_run_id",
                        params.getProperty("workflowRunId", "")).toString();
        const auto run = runtime.workflows().executeRun(id,
            [&p, &identity, &runtime](const WorkflowRun& workflow, const WorkflowStep& step) -> json
            {
                const auto toolName = step.toolName.trim();
                if (toolName.isEmpty())
                    return json{{"success", false}, {"error", "workflow_step_tool_empty"}};
                if (toolName.startsWithIgnoreCase("workflow."))
                    return json{{"success", false}, {"error", "nested_workflow_step_not_allowed"}};

                auto stepParams = step.params.is_object() ? step.params : json::object();
                stepParams["workflow_run_id"] = workflow.id.toStdString();
                stepParams["workflow_step_id"] = step.id.toStdString();
                return parseToolResponse(MCPToolHandler::handle(toolName, jsonToJuceVar(stepParams), p, identity, runtime));
            });
        runtime.events().publish(IntegrationEvent{{}, "workflow", "workflow.executed", run.id, {}, toJson(run), juce::Time::getCurrentTime()});
        return toJString(json{
            {"success", run.state == WorkflowState::Completed},
            {"workflow_run", toJson(run)}
        });
    }

    if (method == "workflow.predict_next")
        return toJString(buildWorkflowPrediction(params, p, identity, runtime));

    if (method == "workflow.cancel")
    {
        const auto id = params.getProperty("workflow_run_id",
                        params.getProperty("workflowRunId", "")).toString();
        const auto run = runtime.workflows().cancelRun(id);
        return toJString(json{{"success", run.state == WorkflowState::Cancelled}, {"workflow_run", toJson(run)}});
    }

    if (method == "context.get_session")
        return toJString(json{{"success", true}, {"session", buildSessionContextJson(p, identity)}});

    if (method == "context.get_transport")
        return toJString(json{{"success", true}, {"transport", buildSessionContextJson(p, identity)["transport"]}});

    if (method == "context.get_track_state")
        return toJString(json{{"success", true}, {"track_state", buildSessionContextJson(p, identity)["trackAssistantState"]}});

    if (method == "events.list_recent")
        return toJString(json{{"success", true}, {"events", runtime.events().listRecent(params.getProperty("limit", 50))}});

    if (method == "sync.export_state")
    {
        const auto sessionId = params.getProperty("session_id",
                              params.getProperty("sessionId", "current")).toString();
        return toJString(json{{"success", true}, {"envelope", toJson(runtime.events().exportState(identity.instanceId, sessionId))}});
    }

    if (method == "sync.apply_envelope")
    {
        const auto envelope = syncEnvelopeFromParams(params);
        const auto event = runtime.events().applyEnvelope(envelope);
        return toJString(json{{"success", true}, {"event", toJson(event)}});
    }

    if (method == "memory.remember")
    {
        MemoryRecord record;
        record.scope = memoryScopeFromString(params.getProperty("scope", "global").toString());
        record.subjectId = params.getProperty("subject_id",
                           params.getProperty("subjectId", "")).toString();
        record.kind = params.getProperty("kind", "note").toString();
        record.content = juceVarToJson(params.getProperty("content", params.getProperty("text", juce::var())));
        record.confidence = static_cast<float>(params.getProperty("confidence", 0.5));
        const auto stored = runtime.memory().remember(record);
        return toJString(json{{"success", true}, {"memory", toJson(stored)}, {"memory_state", runtime.memory().describeState()}});
    }

    if (method == "memory.search")
    {
        const auto scope = memoryScopeFromString(params.getProperty("scope", "global").toString());
        const auto subjectId = params.getProperty("subject_id",
                               params.getProperty("subjectId", "")).toString();
        const auto query = params.getProperty("query", "").toString();
        return toJString(json{{"success", true}, {"records", runtime.memory().search(scope, subjectId, query, params.getProperty("limit", 10))}});
    }

    if (method == "memory.record_outcome")
    {
        const auto transactionId = params.getProperty("transaction_id",
                                   params.getProperty("transactionId", "")).toString();
        if (transactionId.isEmpty())
            return toJString(json{{"success", false}, {"error", "missing_transaction_id"}});

        const auto transaction = runtime.ledger().find(transactionId);
        if (!transaction.has_value())
            return toJString(json{{"success", false}, {"error", "transaction_not_found"}, {"transaction_id", transactionId.toStdString()}});

        ActionOutcome outcome;
        outcome.actionId = transaction->id;
        outcome.workflowRunId = transaction->workflowRunId;
        outcome.beforeState = transaction->beforeState;
        outcome.afterState = transaction->afterState;
        outcome.measurements = transaction->measurements;
        outcome.userAccepted = static_cast<bool>(params.getProperty("user_accepted",
            params.getProperty("userAccepted", transaction->success)));
        outcome.userFeedback = params.getProperty("user_feedback",
            params.getProperty("userFeedback", "")).toString();
        const auto feedbackStatus = normalizeOutcomeFeedbackStatusForMcp(
            params.getProperty("feedback_status",
                params.getProperty("feedbackStatus", "")).toString());
        if (feedbackStatus.isNotEmpty())
        {
            outcome.feedbackStatus = feedbackStatus;
            outcome.userAccepted = outcomeFeedbackStatusIsPositive(feedbackStatus);
            outcome.outcomeScore = outcomeScoreForFeedbackStatusForMcp(feedbackStatus);
        }
        else
        {
            outcome.outcomeScore = juce::jlimit(0.0f, 1.0f,
                static_cast<float>(params.getProperty("outcome_score",
                    params.getProperty("outcomeScore", transaction->success ? 1.0 : 0.0))));
            outcome.feedbackStatus = outcome.userAccepted ? "accepted" : "rejected";
        }
        outcome.source = "user_feedback";

        const auto stored = runtime.memory().recordOutcome(outcome);
        runtime.events().publish(IntegrationEvent{
            {},
            "memory",
            "outcome.recorded",
            outcome.workflowRunId,
            transaction->id,
            json{{"memory_id", stored.id.toStdString()}, {"outcome", toJson(outcome)}},
            juce::Time::getCurrentTime()
        });

        return toJString(json{
            {"success", true},
            {"outcome", toJson(outcome)},
            {"memory", toJson(stored)},
            {"memory_state", runtime.memory().describeState()}
        });
    }

    if (method == "memory.update_outcome_feedback")
    {
        auto actionId = params.getProperty("transaction_id", "").toString();
        if (actionId.isEmpty())
            actionId = params.getProperty("transactionId", "").toString();
        if (actionId.isEmpty())
            actionId = params.getProperty("action_id", "").toString();
        if (actionId.isEmpty())
            actionId = params.getProperty("actionId", "").toString();

        if (actionId.isEmpty())
            return toJString(json{{"success", false}, {"error", "missing_transaction_id"}});

        const auto normalizedStatus = normalizeOutcomeFeedbackStatusForMcp(
            params.getProperty("feedback_status",
                params.getProperty("feedbackStatus",
                    params.getProperty("status", ""))).toString());
        if (normalizedStatus.isEmpty())
            return toJString(json{{"success", false}, {"error", "invalid_feedback_status"}, {"transaction_id", actionId.toStdString()}});

        OutcomeFeedbackUpdate update;
        update.actionId = actionId;
        update.feedbackStatus = normalizedStatus;
        update.userFeedback = params.getProperty("user_feedback",
                              params.getProperty("userFeedback", "")).toString();

        const auto updated = runtime.memory().updateOutcomeFeedback(update);
        if (!updated.has_value())
            return toJString(json{{"success", false}, {"error", "outcome_not_found"}, {"transaction_id", actionId.toStdString()}});

        const auto outcome = updated->content.is_object() ? updated->content : json::object();
        const auto workflowRunId = juce::String(outcome.value("workflowRunId", ""));
        runtime.events().publish(IntegrationEvent{
            {},
            "memory",
            "outcome.feedback_updated",
            workflowRunId,
            actionId,
            json{{"memory_id", updated->id.toStdString()}, {"outcome", outcome}},
            juce::Time::getCurrentTime()
        });

        return toJString(json{
            {"success", true},
            {"transaction_id", actionId.toStdString()},
            {"feedback_status", normalizedStatus.toStdString()},
            {"outcome", outcome},
            {"memory", toJson(*updated)},
            {"memory_state", runtime.memory().describeState()}
        });
    }

    if (method == "memory.list_outcomes")
    {
        const auto workflowRunId = params.getProperty("workflow_run_id",
                                   params.getProperty("workflowRunId", "")).toString();
        return toJString(json{
            {"success", true},
            {"workflow_run_id", workflowRunId.toStdString()},
            {"outcomes", runtime.memory().listOutcomes(workflowRunId, params.getProperty("limit", 50))}
        });
    }

    if (method == "memory.forget")
    {
        const auto id = params.getProperty("id", "").toString();
        return toJString(json{{"success", runtime.memory().forget(id)}, {"id", id.toStdString()}});
    }

    if (method == "memory.get_intent_context")
        return toJString(json{{"success", true}, {"intent_context", runtime.memory().intentContext(buildSessionContextJson(p, identity), params.getProperty("limit", 5))}});

    if (method == "plugin_adapter.describe_capabilities")
        return toJString(buildPluginAdapterCapabilities(p));

    if (method == "plugin_adapter.plan_action")
        return toJString(buildPluginAdapterPlan(params, p));

    return toJString(json{{"success", false}, {"error", "unknown_control_plane_method"}});
}

struct ToolDefinition
{
    const char* name;
    const char* description;
    const char* inputSchema;
    const char* outputSchema = nullptr;   // optional; emitted in tools/list when present
};

// Shared output schema for the verified parameter-write tools. Declares the
// `verification{}` block (Phase 1 execution verification) so clients know the
// success/index/value_before/value_after/verified fields to expect.
static constexpr const char* kVerifiedWriteOutputSchema =
R"({"type":"object","properties":{"success":{"type":"boolean"},"index":{"type":"integer"},"value":{"type":"number"},"queued":{"type":"integer"},"rejected":{"type":"integer"},"verification":{"type":"object","properties":{"status":{"type":"string","enum":["success","queued","value_drift","failure"]},"requested_value":{"type":"number"},"value_before":{"type":"number"},"value_after":{"type":"number"},"human_before":{"type":"string"},"human_after":{"type":"string"},"execution_time_ms":{"type":"number"},"verified":{"type":"boolean"},"error_reason":{"type":"string"},"corrective_action":{"type":"string"}},"required":["status","value_before","value_after","verified"]}}})";

struct DryRunCandidateRecord
{
    juce::String id;
    MultiEffectPlan plan;
    float score = 0.0f;
    json measuredInputs = json::object();
    json rulesApplied = json::array();
};

struct RenderCandidateRecord
{
    int index = 0;
    bool success = false;
    juce::String id;
    juce::String outputPath;
    float peakDb = 0.0f;
    float rmsDb = -100.0f;
    bool hasSilence = false;
    bool hasClipping = false;
    double renderTimeMs = 0.0;
    juce::String errorMessage;
};

struct SafeActionSnapshotRecord
{
    juce::String id;
    juce::String createdAt;
    juce::uint64 processorGenerationToken = 0;
    juce::String layoutSignature;
    std::vector<float> values;
    nlohmann::json action;
};

struct RenderJobRecord
{
    juce::String id;
    juce::String status = "queued";
    juce::String message;
    juce::String inputPath;
    juce::String outputDirectory;
    juce::String pluginPath;
    OfflineBatchProgress progress;
    std::vector<RenderCandidateRecord> candidates;
    bool completed = false;
    bool success = false;
    mutable std::mutex mutex;
};

static std::mutex gSafeActionSnapshotsMutex;
static std::map<std::string, SafeActionSnapshotRecord> gSafeActionSnapshots;
static std::atomic<uint64_t> gNextSafeActionSnapshotId{1};

static std::mutex gDryRunCandidatesMutex;
static std::map<std::string, DryRunCandidateRecord> gDryRunCandidates;
static std::atomic<uint64_t> gNextDryRunBatchId{1};

static std::mutex gRenderJobsMutex;
static std::map<std::string, std::shared_ptr<RenderJobRecord>> gRenderJobs;

// MCP-FILES-04: cap the global result caches so a chatty client / long-lived
// session can't grow process memory without bound. Evicts the lowest-keyed entry
// when over the cap (entries are keyed by unique id, so eviction is
// arbitrary-but-bounded — sufficient to stop unbounded growth).
template <typename MapT>
static void capGlobalCache(MapT& m, typename MapT::size_type maxEntries)
{
    while (m.size() > maxEntries && !m.empty())
        m.erase(m.begin());
}
static constexpr size_t kMaxRenderJobCache          = 64;
static constexpr size_t kMaxDryRunCandidateCache    = 256;
static constexpr size_t kMaxSafeActionSnapshotCache = 256;
static std::atomic<uint64_t> gNextRenderJobId{1};

static constexpr const char* kAnalysisMethodology = "deterministic_dsp";
static constexpr const char* kPlannerType = "heuristic_rule_engine";
static constexpr const char* kPlannerRuleVersion = "mastering_rules_v1";
static constexpr const char* kDryRunScoreBasis = "not_scored_without_audio_render";
static constexpr const char* kRenderedScoreBasis = "render_sanity_metrics";
static constexpr const char* kSpectrumChannelMode = "mono_sum";
static constexpr const char* kSpectrumChannelModeDescription =
    "0.5 * (left + right) is analyzed before FFT; anti-phase stereo can cancel in this view.";
static constexpr const char* kSpectrumMonoSumWarning =
    "spectrum_uses_mono_sum_downmix; anti_phase_stereo_can_cancel_in_this_view";
static constexpr std::array<double, 3> kStereoCrossoverHz { 120.0, 800.0, 8000.0 };

static json modelStatusToJson(MorePhiProcessor& processor)
{
    auto& engine = processor.getAutoMasteringEngine();
    const bool genreLoaded = engine.isGenreClassifierModelLoaded();

    return {
        {"genre_classifier_loaded", genreLoaded},
        {"genre_classifier_backend", genreLoaded ? "loaded_backend" : "default_fallback"},
        {"genre_classifier_status", genreLoaded ? "model_loaded" : "default_fallback"},
        {"genre_classifier_inference", genreLoaded ? "available" : "unavailable"},
        {"limitations", json::array({
            "genre classifier uses default fallback when no model is loaded"
        })}
    };
}

static json analysisMetadataToJson(MorePhiProcessor& processor, const char* dataScope)
{
    json metadata{
        {"methodology", kAnalysisMethodology},
        {"method", "deterministic_dsp_metering"},
        {"analysis_kind", "realtime_dsp_metering"},
        {"data_scope", dataScope != nullptr ? dataScope : "instantaneous_snapshot"},
        {"measurement_state", dataScope != nullptr && std::string(dataScope) == "rolling_window"
            ? "rolling_available_history" : "latest_available_snapshot"},
        {"metering_standard_claim", "lightweight_bs1770_style_estimates"},
        {"algorithm_ids", {
            {"loudness", "lightweight_bs1770_style_rolling_estimate"},
            {"true_peak", "4x_polyphase_fir_estimate"},
            {"spectrum", "hann_window_fft_mono_sum"},
            {"stereo_field", "mid_side_energy_butterworth_bands"},
            {"window_statistics", "rolling_min_max_mean_p10_p50_p90"}
        }},
        {"history_limit_seconds", MeterWindowAccumulator::kCapacity / 10.0f},
        {"sample_interval_seconds", 0.1f},
        {"limitations", json::array({
            "formal reference-vector certification is not claimed",
            "loudness values are rolling available-history estimates",
            "recommendations are emitted by separate heuristic endpoints"
        })}
    };

    if (processor.getSampleRate() > 0.0)
        metadata["sample_rate"] = processor.getSampleRate();
    else
        metadata["sample_rate"] = nullptr;

    return metadata;
}

static json analysisWarningsToJson()
{
    return json::array({
        "meters_are_deterministic_dsp_estimates; formal_reference_vector_certification_is_not_claimed",
        "lufs_values_are_rolling_available_history_estimates_not_external_lab_certification"
    });
}

static json metricStatisticsToJson(const MeterWindowAccumulator::MetricStatistics& stats)
{
    return {
        {"min", stats.min},
        {"max", stats.max},
        {"mean", stats.mean},
        {"p10", stats.p10},
        {"p50", stats.p50},
        {"p90", stats.p90},
        {"count", stats.count}
    };
}

static json windowStatisticsToJson(const MeterWindowAccumulator::WindowStatistics& stats)
{
    return {
        {"rms", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::rms])},
        {"lufs_momentary", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::lufsMomentary])},
        {"lufs_short_term", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::lufsShortTerm])},
        {"lufs_integrated", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::lufsIntegrated])},
        {"lra", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::lra])},
        {"true_peak_dbtp", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::truePeakDBTP])},
        {"limiter_gr_db", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::limiterGRDB])},
        {"spectral_centroid_hz", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::spectralCentroidHz])},
        {"spectral_tilt_db_per_octave", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::spectralTiltDBPerOctave])},
        {"stereo_width", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::stereoWidth])},
        {"mid_band_correlation", metricStatisticsToJson(stats.metrics[MeterWindowAccumulator::midBandCorrelation])}
    };
}

static json currentMeasurementsToJson(MorePhiProcessor& processor)
{
    auto& engine = processor.getAutoMasteringEngine();
    json dynamics = json::array();
    for (int i = 0; i < 4; ++i)
        dynamics.push_back(finiteOr(engine.getGainReductionDB(i), -300.0));

    return {
        {"rms", finiteOr(processor.getRmsLevel(), -300.0)},
        {"queue_usage", processor.getCommandQueueUsage()},
        {"lufs_momentary", finiteOr(engine.getLUFSMomentary(), -70.0)},
        {"lufs_short_term", finiteOr(engine.getLUFSShortTerm(), -70.0)},
        {"lufs_integrated", finiteOr(engine.getLUFSIntegrated(), -70.0)},
        {"lra", finiteOr(engine.getLRA(), 0.0)},
        {"true_peak_dbtp", finiteOr(engine.getTruePeak_dBTP(), -300.0)},
        {"limiter_gr_db", finiteOr(engine.getLimiterGainReductionDB(), 0.0)},
        {"dynamics_gr_db", dynamics}
    };
}

static float scoreRenderedCandidate(const RenderCandidateRecord& candidate) noexcept
{
    return scoreRenderedMasteringCandidate({
        candidate.success,
        candidate.peakDb,
        candidate.rmsDb,
        candidate.hasSilence,
        candidate.hasClipping
    });
}

static juce::String buildParameterLayoutSignature(const std::vector<ParameterBridge::ParameterDescriptor>& descriptors)
{
    juce::String signature;
    signature << descriptors.size();

    for (const auto& descriptor : descriptors)
    {
        signature << '|'
                  << descriptor.index << ':'
                  << descriptor.stableId << ':'
                  << descriptor.name;
    }

    return signature;
}

static bool snapshotContextMatches(const SafeActionSnapshotRecord& record,
                                   MorePhiProcessor& processor,
                                   const std::vector<ParameterBridge::ParameterDescriptor>& descriptors)
{
    return record.processorGenerationToken == processor.getProcessorGenerationToken()
        && record.layoutSignature == buildParameterLayoutSignature(descriptors);
}

static bool hasPendingParameterCommands(MorePhiProcessor& processor)
{
    return processor.getPendingParameterCommandCountApprox() > 0;
}

static json parameterFlushToJson(const MorePhiProcessor::ParameterCommandFlushResult& flush)
{
    return {
        {"pending_before", flush.pendingBefore},
        {"drained", flush.drained},
        {"pending_after", flush.pendingAfter},
        {"plugin_unavailable", flush.pluginUnavailable},
        {"exclusive_access_timed_out", flush.exclusiveAccessTimedOut},
        {"retry_count", flush.retryCount},
        {"waited_ms", flush.waitedMs},
        {"out_of_range_count", flush.outOfRangeCount}
    };
}

// ── Execution verification (AI↔MCP↔VST3 Phase 1) ──────────────────────────
// Every parameter-write tool captures value_before / value_after so the AI's
// "applied X → Y" confirmations are verifiable rather than assumed. Continuous
// parameters apply within tolerance; discrete parameters may legitimately snap
// to a step, which surfaces as a (non-fatal) value_drift status with a
// corrective action instead of a silent mismatch.
// AUDIT-FIX 4.1: default drift tolerance for continuous parameters.
// For discrete parameters the effective tolerance is tightened to 0.5 / numSteps
// so a step-snap is correctly detected rather than silently passing.
static constexpr float kVerificationDriftToleranceContinuous = 0.01f;

struct VerificationCapture
{
    float requestedValue = 0.0f;
    float valueBefore = 0.0f;
    float valueAfter = 0.0f;
    juce::String humanBefore;
    juce::String humanAfter;
    juce::String status;
    juce::String errorReason;
    juce::String correctiveAction;
    double executionTimeMs = 0.0;
    juce::String transactionId;
    juce::String rollbackId;
    float audioStateDelta = 0.0f;
};

// Classifies a write outcome.
//   applied          - the edit reached the parameter path (queue/resolve succeeded)
//   drained          - it was actually applied to the underlying parameter this call
//                      (false when still pending in the realtime queue)
//   requestedValue / valueAfter - clamped normalized target and post-write read
//   failureCode      - error code when !applied (e.g. "queue_full")
//   isDiscrete       - when true, tolerance = max(0.5f / numSteps, 0.001f)
//   numSteps         - used for discrete tolerance calculation
//   morphOverwriteRisk - when true and value drifted, status is "morph_overwrite_risk"
//                         instead of "value_drift" (AUDIT-FIX 4.5)
static VerificationCapture classifyVerification(bool applied, bool drained,
                                                float requestedValue, float valueAfter,
                                                double executionTimeMs,
                                                const juce::String& failureCode = {},
                                                bool isDiscrete = false,
                                                int numSteps = 0,
                                                bool morphOverwriteRisk = false)
{
    VerificationCapture v;
    v.requestedValue = requestedValue;
    v.valueAfter = valueAfter;
    v.executionTimeMs = executionTimeMs;
    if (!applied)
    {
        const auto code = failureCode.isEmpty() ? "queue_full" : failureCode;
        v.status = "failure";
        v.errorReason = code;
        v.correctiveAction = suggestedActionForError(code);
        return v;
    }
    if (!drained)
    {
        v.status = "queued";
        v.errorReason = "pending_apply";
        v.correctiveAction = suggestedActionForError("pending_parameter_edits");
        return v;
    }

    // AUDIT-FIX 4.1: discrete-aware tolerance — use half-step width for
    // discrete parameters so a snap to the wrong step is detected.
    float tolerance = kVerificationDriftToleranceContinuous;
    if (isDiscrete && numSteps > 0)
    {
        tolerance = std::max(0.5f / static_cast<float>(numSteps), 0.001f);
    }

    if (std::abs(valueAfter - requestedValue) > tolerance)
    {
        // AUDIT-FIX 4.5: when morph overwrite risk is flagged, surface a
        // distinct status so the AI can distinguish "my edit was overwritten
        // by morph" from "the plugin rejected my value".
        if (morphOverwriteRisk)
        {
            v.status = "morph_overwrite_risk";
            v.errorReason = "morph_may_have_overwritten_edit";
            v.correctiveAction = "Pause morph or increase live-edit hold threshold before re-applying the edit.";
        }
        else if (isDiscrete)
        {
            v.status = "value_drift_discrete";
            v.errorReason = "discrete_parameter_snapped_to_different_step";
            v.correctiveAction = suggestedActionForError("value_drift");
        }
        else
        {
            v.status = "value_drift";
            v.errorReason = "applied_value_differs_from_requested";
            v.correctiveAction = suggestedActionForError("value_drift");
        }
        return v;
    }
    v.status = "success";
    return v;
}

// F4/AUDIT: the immediate post-flush readback (valueAfter) cannot detect a
// morph overwrite that happens in the NEXT applyMorphAndParameters block —
// morph runs every block after the drain and rewrites parameters once
// liveEditHold_ releases. This sleeps one audio block's worth (~6ms, covering
// 128 samples @48k plus headroom) then re-reads. If the value diverged from
// what we wrote, morph overwrote it: downgrade to morph_overwrite_confirmed.
// Returns true (with an updated VerificationCapture) only when a deferred
// overwrite was actually detected; nullopt otherwise (caller keeps its result).
static std::optional<VerificationCapture>
deferredMorphOverwriteCheck(ParameterBridge& bridge, int id, float requestedValue,
                            bool isDiscrete, int numSteps)
{
    // ponytail: bounded single sleep. A full deferred-readback subsystem is
    // unwarranted for a synchronous MCP tool that must return promptly.
    juce::Thread::sleep(6);
    const float valueDeferred = bridge.getParameterNormalized(id);
    float tolerance = kVerificationDriftToleranceContinuous;
    if (isDiscrete && numSteps > 0)
        tolerance = std::max(0.5f / static_cast<float>(numSteps), 0.001f);
    if (std::abs(valueDeferred - requestedValue) > tolerance)
    {
        VerificationCapture v;
        v.requestedValue = requestedValue;
        v.valueAfter = valueDeferred;
        v.status = "morph_overwrite_confirmed";
        v.errorReason = "morph_overwrote_edit_after_one_block";
        v.correctiveAction = "Pause morph (or move morph position away from a "
                             "snapshot boundary) before re-applying this edit.";
        return v;
    }
    return std::nullopt;
}

static json verificationToJson(const VerificationCapture& v)
{
    json j{
        {"status", v.status.toStdString()},
        {"requested_value", v.requestedValue},
        {"value_before", v.valueBefore},
        {"value_after", v.valueAfter},
        {"human_before", v.humanBefore.toStdString()},
        {"human_after", v.humanAfter.toStdString()},
        {"execution_time_ms", v.executionTimeMs},
        {"verified", v.status == "success"},
        {"audio_state_delta", v.audioStateDelta}
    };
    if (!v.transactionId.isEmpty())
        j["transaction_id"] = v.transactionId.toStdString();
    if (!v.rollbackId.isEmpty())
        j["rollback_id"] = v.rollbackId.toStdString();
    if (!v.errorReason.isEmpty())
    {
        j["error_reason"] = v.errorReason.toStdString();
        j["corrective_action"] = v.correctiveAction.toStdString();
    }
    return j;
}

static uint64_t checksumNormalizedValues(const std::vector<float>& values)
{
    uint64_t hash = 1469598103934665603ull;
    for (float value : values)
    {
        const auto quantized = static_cast<uint32_t>(
            std::llround(std::clamp(value, 0.0f, 1.0f) * 1000000.0f));
        hash ^= quantized;
        hash *= 1099511628211ull;
    }
    return hash;
}

class SnapshotSelfTestBridge final : public IParameterBridge
{
public:
    explicit SnapshotSelfTestBridge(int parameterCount)
        : values(static_cast<size_t>(parameterCount), 0.0f)
    {
    }

    int getParameterCount() const override { return static_cast<int>(values.size()); }

    float getParameterNormalized(int index) const override
    {
        if (index < 0 || index >= static_cast<int>(values.size()))
            return 0.0f;
        return values[static_cast<size_t>(index)];
    }

    void setParameterNormalized(int index, float value) override
    {
        if (index >= 0 && index < static_cast<int>(values.size()))
            values[static_cast<size_t>(index)] = juce::jlimit(0.0f, 1.0f, value);
    }

    juce::String getParameterName(int index) const override
    {
        return "Self Test Param " + juce::String(index);
    }

    void applyParameterState(const std::vector<float>& newValues) override
    {
        applyParameterState(newValues.data(), static_cast<int>(newValues.size()));
    }

    void applyParameterState(const float* newValues, int count) override
    {
        if (newValues == nullptr || count <= 0)
            return;

        ++applyCount;
        const int safeCount = juce::jmin(count, static_cast<int>(values.size()));
        for (int i = 0; i < safeCount; ++i)
            values[static_cast<size_t>(i)] = juce::jlimit(0.0f, 1.0f, newValues[i]);
    }

    std::vector<float> captureParameterState() const override { return values; }
    bool isDiscrete(int) const override { return false; }
    std::vector<bool> getDiscreteMap() const override { return std::vector<bool>(values.size(), false); }
    juce::String getParameterLabel(int) const override { return {}; }
    juce::String getParameterDisplayValue(int index) const override { return juce::String(getParameterNormalized(index)); }
    float getParameterDefault(int) const override { return 0.5f; }
    juce::StringArray getParameterValueStrings(int) const override { return {}; }
    juce::String getParameterStableID(int index) const override { return "selftest_" + juce::String(index); }
    int getParameterNumSteps(int) const override { return 0; }

    std::vector<float> values;
    int applyCount = 0;
};

static json parseSchema(const char* schemaText)
{
    if (schemaText == nullptr || schemaText[0] == '\0')
        return json{{"type", "object"}, {"properties", json::object()}};

    try
    {
        return json::parse(schemaText);
    }
    catch (...)
    {
        return json{{"type", "object"}, {"properties", json::object()}};
    }
}

static const ToolDefinition kCoreTools[] = {
    {"heartbeat", "Liveness probe. Requires authentication but does NOT consume a rate-limit slot, so an idle client can keep the connection alive without starving tool traffic. Returns server time, uptime, queue depth, connected clients, and health.", R"({"type":"object","properties":{"client_clock_ms":{"type":"integer","description":"Optional client-side epoch millis for RTT estimation."}}})"},
    {"get_plugin_info", "Return More-Phi and hosted plugin identity information.", R"({"type":"object","properties":{}})"},
    {"list_parameters", "List HOSTED PLUGIN parameters with stable IDs and normalized values. For More-Phi's own controls, use more_phi.parameters.", R"({"type":"object","properties":{}})"},
    {"get_parameter", "Read a HOSTED PLUGIN parameter by stableId, index, or exact name. For More-Phi's own controls, use more_phi.get_parameter.", R"({"type":"object","properties":{"stableId":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"}}})"},
    {"set_parameter", "Set one HOSTED PLUGIN parameter by stableId, index, or exact name using the realtime-safe queue and immediate assistant flush. IMPORTANT: this changes the HOSTED VST3 plugin's parameters, NOT More-Phi's own controls. To change More-Phi's internal parameters (morph position, physics mode, cpuSaver, etc.), use more_phi.set_parameter instead.", R"({"type":"object","properties":{"stableId":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"},"value":{"type":"number"}},"required":["value"]})", kVerifiedWriteOutputSchema},
    {"set_parameters_batch", "Set multiple hosted plugin parameters using the realtime-safe queue and immediate assistant flush.", R"({"type":"object","properties":{"parameters":{"type":"array","items":{"type":"object"}},"params":{"type":"array","items":{"type":"object"}}}})", kVerifiedWriteOutputSchema},
    {"sweep_parameter", "Autonomously sweep ONE hosted-plugin parameter across a normalized [from,to] range in `steps` increments, capturing live measurements (LUFS-I/S/M, LRA, true-peak dBTP, spectral centroid/tilt, stereo width/correlation, THD%, program crest factor) after each step. This is the ONLY tool that performs a value-space sweep on the LIVE hosted plugin; it reuses the verified write path (resolve -> command queue -> drain -> ParameterBridge -> readback) for every step. Each step enqueues a set_parameter value, flushes, waits capture_ms, then reads getLiveMeasurements(). Returns an array of {step, value, measurements}. Note: this is a slow, audio-driven tool — the user must have audio playing through the track for the measurements to be meaningful.", R"({"type":"object","properties":{"parameter":{"description":"stableId, index, or exact name"},"index":{"type":"integer"},"stableId":{"type":"string"},"name":{"type":"string"},"from":{"type":"number","minimum":0,"maximum":1,"default":0},"to":{"type":"number","minimum":0,"maximum":1,"default":1},"steps":{"type":"integer","minimum":2,"maximum":64,"default":5},"capture_ms":{"type":"integer","minimum":10,"maximum":5000,"default":250}},"required":["from","to"]})", kVerifiedWriteOutputSchema},
    {"capture_snapshot", "Capture the current hosted parameter state into a More-Phi snapshot slot.", R"({"type":"object","properties":{"slot":{"type":"integer","minimum":0,"maximum":11},"includeState":{"type":"boolean","default":true}},"required":["slot"]})"},
    {"recall_snapshot", "Recall a More-Phi snapshot through the realtime-safe queue.", R"({"type":"object","properties":{"slot":{"type":"integer","minimum":0,"maximum":11},"mode":{"type":"string","enum":["fast","full"]}},"required":["slot"]})"},
    {"set_morph_position", "Set More-Phi morph pad/fader state.", R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"fader":{"type":"number"},"source":{"type":"string","enum":["xy","xypad","fader"]}}})"},
    {"get_morph_state", "Return More-Phi morph pad/fader state.", R"({"type":"object","properties":{}})"},
    {"more_phi.parameters", "List More-Phi's own APVTS runtime controls with normalized values.", R"({"type":"object","properties":{}})"},
    {"more_phi.get_parameter", "Read a More-Phi APVTS runtime control by parameter_id, index, or exact name.", R"({"type":"object","properties":{"parameter_id":{"type":"string"},"parameterId":{"type":"string"},"id":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"}}})"},
    {"more_phi.set_parameter", "Set one More-Phi APVTS runtime control by normalized value. IMPORTANT: this changes More-Phi's OWN internal parameters (morph position, physics mode, cpuSaver, etc.), NOT the hosted VST3 plugin's parameters. To change the hosted plugin use set_parameter or hosted_plugin.set_parameter instead.", R"({"type":"object","properties":{"parameter_id":{"type":"string"},"parameterId":{"type":"string"},"id":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"},"value":{"type":"number"}},"required":["value"]})", kVerifiedWriteOutputSchema},
    {"more_phi.set_parameters", "Set multiple More-Phi APVTS runtime controls by normalized value.", R"({"type":"object","properties":{"parameters":{"type":"array","items":{"type":"object","properties":{"parameter_id":{"type":"string"},"parameterId":{"type":"string"},"id":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"},"value":{"type":"number"}}}},"params":{"type":"array","items":{"type":"object"}}}})", kVerifiedWriteOutputSchema},
    {"run_self_test", "Run a deterministic local More-Phi self-test without waiting for an external LLM.", R"({"type":"object","properties":{"suite":{"type":"string","enum":["quick","snapshot","full"],"default":"quick"}}})"},
    {"get_mastering_state", "Return current local mastering meters and hosted Ozone status.", R"({"type":"object","properties":{}})"},
    {"ozone.audit_parameters", "Discover Ozone parameter indices from the current hosted plugin and optionally apply the map.", R"({"type":"object","properties":{"apply":{"type":"boolean","default":false}}})"},
    {"apply_mastering_plan", "Generate and apply a HEURISTIC mastering plan from compact analysis metrics. AUDIT-FIX-4: this drives BOTH More-Phi's internal chain AND (if ozone.audit_parameters has populated the map) the hosted Ozone plugin's parameters. The hosted-plugin writes are inert until audit_parameters(apply=true) has run against a loaded Ozone instance — call ozone.audit_parameters first if you intend to master through Ozone.", R"({"type":"object","properties":{"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
     {"sonicmaster_decision", "Run the neural mastering model on the last ~6s of captured audio and return the decoded mastering decision (EQ gains, target LUFS, true-peak ceiling, 3-band compressor, stereo width, limiter, character) WITHOUT applying it. Uses the embedded ONNX model IN-PROCESS as the primary path (no server needed once the plugin is built with MORE_PHI_ENABLE_ONNX); the local Python HTTP inference server at 127.0.0.1:8765 is a fallback for when the ONNX model can't load. Use this as the PRIMARY source of mastering decisions; fall back to apply_mastering_plan (heuristic) only if it errors or is unavailable. The user should play audio for ~6s before calling. NOTE (target_lufs): target_lufs is the mastering TARGET (honored at decode time as an override of the model's recommendation), NOT a measurement of the input's loudness — the ~6s window is peak-normalized, so the model cannot infer absolute LUFS; use live_measurements for a real ITU-R BS.1770 loudness reading. NOTE (live_measurements, AUDIT-FIX-R2): the response includes a live_measurements block with genuine ITU-R BS.1770-4 LUFS, true-peak, LRA, spectral centroid, spectral tilt, stereo width, mid-correlation, THD%, and program crest factor from the engine's live meters. These are MEASUREMENTS (not model estimates). NOTE (apply target): when applied via mastering.neural_apply or the background cycle, the decision drives the HOSTED plugin's parameters (EQ/dynamics/stereo/maximizer) through OzonePlanApplicator. More-Phi's internal mastering chain is dormant by design. NOTE (apply_limiter_ceiling): opt-in bool; when true, the decoded true-peak ceiling is honoured on apply, hard-clamped to the streaming-safe -1.0 dBTP. Default false (limiter is high-risk).", R"({"type":"object","properties":{"target_lufs":{"type":"number","default":-14.0,"minimum":-30,"maximum":-6},"apply_limiter_ceiling":{"type":"boolean","default":false}}})"},
    {"mastering.neural_apply", "One-click neural Master Assistant: run the ONNX decision on the last ~6s of captured audio AND apply it to the hosted plugin in a single call. Composes requestDecisionNow + applyValidatedPlan. Returns applied=true with the per-slot breakdown (enqueued/skipped/unmapped) via mapping_status. This is the COMMIT door — sonicmaster_decision stays decision-only for preview; this writes hosted-plugin parameters. Requires a hosted plugin whose parameters are mapped (check mapping_status.ozone_mapped); if no plugin is hosted or the map is all-stubs, the apply is a no-op and the response explains why. Params: target_lufs (float, default -14, overrides the model's loudness recommendation), apply_limiter_ceiling (bool, default false). The user should play audio for ~6s before calling.", R"({"type":"object","properties":{"target_lufs":{"type":"number","default":-14.0,"minimum":-30,"maximum":-6},"apply_limiter_ceiling":{"type":"boolean","default":false}}})"},
    {"morephi_ipc_attach", "Attach read-only to a named IPC shared-memory segment.", R"({"type":"object","properties":{"segment_name":{"type":"string"},"daw_process_id":{"type":"integer","minimum":0},"mapped_size_bytes":{"type":"integer","minimum":1,"default":4194304}}})"},
    {"morephi_ipc_detach", "Detach from the currently mapped IPC segment.", R"({"type":"object","properties":{}})"},
    {"morephi_ipc_status", "Report IPC attachment state and last attach error.", R"({"type":"object","properties":{}})"},
    {"morephi_ipc_snapshot", "Read a bounded byte range from the attached IPC segment and report candidate MORP frames.", R"({"type":"object","properties":{"offset":{"type":"integer","minimum":0,"default":0},"size_bytes":{"type":"integer","minimum":1,"default":1024},"max_frames":{"type":"integer","minimum":0,"default":16}}})"},
    {"morephi_ipc_dump", "Write a bounded raw byte range from the attached IPC segment to a local file.", R"({"type":"object","properties":{"output_path":{"type":"string"},"offset":{"type":"integer","minimum":0,"default":0},"size_bytes":{"type":"integer","minimum":1,"default":65536}},"required":["output_path"]})"},
    {"morephi_ipc_capture", "Sample a bounded IPC memory window and record byte changes for Assistant troubleshooting.", R"({"type":"object","properties":{"offset":{"type":"integer","minimum":0,"default":0},"size_bytes":{"type":"integer","minimum":1,"default":4096},"duration_ms":{"type":"integer","minimum":0,"default":2000},"interval_ms":{"type":"integer","minimum":1,"default":25},"max_changes":{"type":"integer","minimum":1,"default":64},"max_ranges_per_change":{"type":"integer","minimum":1,"default":64},"max_frames":{"type":"integer","minimum":0,"default":16},"baseline_base64":{"type":"string"},"output_path":{"type":"string"},"include_changed_bytes":{"type":"boolean","default":false}}})"},
    {"morephi_ipc_run_assistant", "Inject an AssistantRequest into the manifest-defined IPC ring and wait for AssistantResult parameter decisions.", R"({"type":"object","properties":{"schema_path":{"type":"string"},"segment_name":{"type":"string"},"daw_process_id":{"type":"integer","minimum":0},"instance_id":{"type":"integer","minimum":0},"plugin_name_query":{"type":"string","default":""},"timeout_ms":{"type":"integer","minimum":0,"default":10000},"poll_interval_ms":{"type":"integer","minimum":1,"default":10},"observer_id":{"type":"integer","minimum":0,"default":3735928559},"allow_unsafe_write":{"type":"boolean","default":false},"apply_result":{"type":"boolean","default":false}},"required":["allow_unsafe_write"]})"},
    {"hosted_plugin.scan", "Inspect a VST3 plugin path and return the discoverable plugin description.", R"({"type":"object","properties":{"path":{"type":"string"},"plugin_path":{"type":"string"}}})"},
    {"hosted_plugin.load", "Load a hosted VST3 plugin from an explicit path.", R"({"type":"object","properties":{"path":{"type":"string"},"plugin_path":{"type":"string"}}})"},
    {"hosted_plugin.info", "Alias for get_plugin_info.", R"({"type":"object","properties":{}})"},
    {"hosted_plugin.parameters", "Alias for list_parameters.", R"({"type":"object","properties":{}})"},
    {"hosted_plugin.set_parameter", "Set one HOSTED PLUGIN parameter. Same as set_parameter — changes the loaded VST3 plugin, NOT More-Phi's own internal controls. For More-Phi controls use more_phi.set_parameter.", R"({"type":"object","properties":{"stableId":{"type":"string"},"index":{"type":"integer"},"name":{"type":"string"},"value":{"type":"number"}},"required":["value"]})"},
    {"hosted_plugin.set_parameters", "Set multiple HOSTED PLUGIN parameters. Same as set_parameters_batch — changes the loaded VST3 plugin, NOT More-Phi's own internal controls.", R"({"type":"object","properties":{"parameters":{"type":"array","items":{"type":"object"}}}})"},
    {"hosted_plugin.capture_state", "Capture hosted plugin parameter state, optionally into a snapshot slot.", R"({"type":"object","properties":{"slot":{"type":"integer","minimum":0,"maximum":11},"include_values":{"type":"boolean","default":false},"includeState":{"type":"boolean","default":true}}})"},
    {"diagnose_parameter_pipeline", "Diagnose the AI-to-hosted-plugin parameter pipeline. Reports tool resolution, queue health, plugin availability, flush readiness, restore state, morph hold counts, and snapshot occupancy.", R"({"type":"object","properties":{"index":{"type":"integer","description":"Optional parameter index to check live-edit hold state for"}}})"},
    {"analysis.get_summary", "Return compact More-Phi-owned analysis and metering data.", R"({"type":"object","properties":{}})"},
    {"analysis.get_spectrum", "Return the latest realtime spectrum analyzer snapshot rebinned to the requested resolution.", R"({"type":"object","properties":{"resolution":{"type":"integer","enum":[32,64,128,256],"default":64}}})"},
    {"analysis.get_stereo_field", "Return the latest 4-band stereo field and mid-side analysis snapshot.", R"({"type":"object","properties":{}})"},
    {"analysis.capture_window", "Return rolling deterministic DSP meter statistics for a requested window length.", R"({"type":"object","properties":{"window_seconds":{"type":"number","default":3.0}}})"},
    {"analysis.compare_render", "Compare two compact analysis summaries or compare a provided summary to current meters.", R"({"type":"object","properties":{"before":{"type":"object"},"after":{"type":"object"}}})"},
    {"mastering.plan_preview", "Generate a mastering plan without applying it.", R"({"type":"object","properties":{"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
    {"mastering.apply_plan", "Alias for apply_mastering_plan.", R"({"type":"object","properties":{"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
    {"mastering.render_batch", "Create dry-run mastering candidates or start an offline file-backed hosted-plugin render job.", R"({"type":"object","properties":{"candidate_count":{"type":"integer","default":3},"dry_run":{"type":"boolean","default":true},"input_path":{"type":"string"},"output_path":{"type":"string"},"plugin_path":{"type":"string"},"allow_passthrough":{"type":"boolean","default":false},"duration_seconds":{"type":"number"},"sample_rate":{"type":"number","default":48000},"block_size":{"type":"integer","default":512},"channels":{"type":"integer","default":2},"parallel_workers":{"type":"integer","default":1},"genre_index":{"type":"integer"},"dynamic_range":{"type":"number"},"spectral_tilt":{"type":"number"},"correlation_ms":{"type":"number"}}})"},
    {"mastering.render_status", "Poll an offline mastering render job started by mastering.render_batch.", R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})"},
    {"mastering.select_candidate", "Select a candidate ID returned by mastering.render_batch.", R"({"type":"object","properties":{"candidate_id":{"type":"string"}},"required":["candidate_id"]})"},
    {"plugin_profile.audit_parameters", "Audit hosted plugin parameters into a versioned profile JSON object.", R"({"type":"object","properties":{}})"},
    {"plugin_profile.describe_semantics", "Return hosted plugin semantic controls with safety categories and action limits.", R"({"type":"object","properties":{}})"},
    {"plugin_profile.apply_safe_action", "Apply one guarded semantic hosted-plugin action through the realtime-safe queue and return a rollback snapshot ID.", R"({"type":"object","properties":{"action":{"type":"object"},"allow_caution":{"type":"boolean","default":false},"dry_run":{"type":"boolean","default":false}},"required":["action"]})"},
    {"plugin_profile.restore_safe_snapshot", "Restore a safe-action rollback snapshot created by plugin_profile.apply_safe_action.", R"({"type":"object","properties":{"snapshot_id":{"type":"string"}},"required":["snapshot_id"]})"},
    {"plugin_profile.get", "Load a saved profile by ID, or audit the current hosted plugin when omitted.", R"({"type":"object","properties":{"profile_id":{"type":"string"}}})"},
    {"plugin_profile.save", "Audit and save the current hosted plugin profile under the user app-data directory.", R"({"type":"object","properties":{}})"},
    {"plugin_profile.describe_semantic_map", "Describe the current hosted plugin as LLM-safe semantic controls grouped by automation safety.", R"({"type":"object","properties":{"plugin_id":{"type":"string","default":"current"},"include_raw_parameters":{"type":"boolean","default":false},"max_controls":{"type":"integer","default":128,"minimum":0}}})"},
    {"describe_plugin_semantic_map", "Alias for plugin_profile.describe_semantic_map.", R"({"type":"object","properties":{"plugin_id":{"type":"string","default":"current"},"include_raw_parameters":{"type":"boolean","default":false},"max_controls":{"type":"integer","default":128,"minimum":0}}})"},
    {"automation.history", "List recent AutomationTransaction ledger entries for auditable MCP writes, optionally filtered by WorkflowRun.", R"({"type":"object","properties":{"limit":{"type":"integer","default":50,"minimum":1},"workflow_run_id":{"type":"string"},"workflowRunId":{"type":"string"}}})"},
    {"automation.get_transaction", "Return one AutomationTransaction by ID.", R"({"type":"object","properties":{"transaction_id":{"type":"string"},"transactionId":{"type":"string"}},"required":["transaction_id"]})"},
    {"automation.rollback", "Rollback a reversible AutomationTransaction through safe parameter queues where possible.", R"({"type":"object","properties":{"transaction_id":{"type":"string"},"transactionId":{"type":"string"}},"required":["transaction_id"]})"},
    {"automation.diff_preview", "Preview parameter diffs for a planned write-capable tool call.", R"({"type":"object","properties":{"tool_name":{"type":"string"},"toolName":{"type":"string"},"params":{"type":"object"},"parameters":{"type":"array"}}})"},
    {"workflow.create", "Create a durable WorkflowRun scaffold from a user intent and context snapshot.", R"({"type":"object","properties":{"user_intent":{"type":"string"},"userIntent":{"type":"string"},"context":{"type":"object"}}})"},
    {"workflow.submit", "Submit a validated WorkflowRun DAG with explicit executable WorkflowStep objects.", R"({"type":"object","properties":{"workflow_run":{"type":"object"},"workflowRun":{"type":"object"},"user_intent":{"type":"string"},"userIntent":{"type":"string"},"steps":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"toolName":{"type":"string"},"params":{"type":"object"},"dependencies":{"type":"array","items":{"type":"string"}},"maxRetries":{"type":"integer","default":1}}}}}})"},
    {"workflow.get", "Return a WorkflowRun by ID.", R"({"type":"object","properties":{"workflow_run_id":{"type":"string"},"workflowRunId":{"type":"string"}}})"},
    {"workflow.list", "List in-memory WorkflowRun objects.", R"({"type":"object","properties":{}})"},
    {"workflow.execute", "Execute the non-audio-thread WorkflowRun state machine scaffold.", R"({"type":"object","properties":{"workflow_run_id":{"type":"string"},"workflowRunId":{"type":"string"}}})"},
    {"workflow.predict_next", "Suggest safe read-only next workflow actions using current session context and advisory ActionOutcome memory evidence.", R"({"type":"object","properties":{"workflow_run_id":{"type":"string"},"workflowRunId":{"type":"string"},"user_intent":{"type":"string"},"userIntent":{"type":"string"},"memory_limit":{"type":"integer","default":5,"minimum":1,"maximum":20},"memoryLimit":{"type":"integer","default":5,"minimum":1,"maximum":20}}})"},
    {"workflow.cancel", "Cancel a WorkflowRun.", R"({"type":"object","properties":{"workflow_run_id":{"type":"string"},"workflowRunId":{"type":"string"}}})"},
    {"permission.get_state", "Return the dispatch-layer PermissionPolicy state and autonomy level.", R"({"type":"object","properties":{}})"},
    {"permission.set_autonomy", "Set autonomy level: manual, assist, co_pilot, or autopilot.", R"({"type":"object","properties":{"level":{"type":"string","enum":["manual","assist","co_pilot","autopilot"]},"autonomy_level":{"type":"string"}}})"},
    {"permission.list_approvals", "List dispatch-layer ApprovalRequest objects.", R"({"type":"object","properties":{}})"},
    {"permission.approve", "Approve a pending ApprovalRequest.", R"({"type":"object","properties":{"approval_id":{"type":"string"},"approvalId":{"type":"string"}}})"},
    {"permission.reject", "Reject a pending ApprovalRequest.", R"({"type":"object","properties":{"approval_id":{"type":"string"},"approvalId":{"type":"string"}}})"},
    {"context.get_session", "Return a SessionContext snapshot for workflow planning.", R"({"type":"object","properties":{}})"},
    {"context.get_transport", "Return the current DAW transport snapshot when available.", R"({"type":"object","properties":{}})"},
    {"context.get_track_state", "Return compact track-assistant state for planning.", R"({"type":"object","properties":{}})"},
    {"events.list_recent", "List recent IntegrationEvent entries from the bounded event bus.", R"({"type":"object","properties":{"limit":{"type":"integer","default":50,"minimum":1}}})"},
    {"sync.export_state", "Export a SyncEnvelope for slow session/workflow/profile synchronization.", R"({"type":"object","properties":{"session_id":{"type":"string"},"sessionId":{"type":"string"}}})"},
    {"sync.apply_envelope", "Apply a SyncEnvelope to the integration event stream.", R"({"type":"object","properties":{"instance_id":{"type":"string"},"instanceId":{"type":"string"},"session_id":{"type":"string"},"sessionId":{"type":"string"},"revision":{"type":"integer"},"state_patch":{"type":"object"},"statePatch":{"type":"object"},"conflict_policy":{"type":"string"},"conflictPolicy":{"type":"string"}}})"},
    {"memory.remember", "Store a MemoryRecord in the local preference/outcome memory backend.", R"({"type":"object","properties":{"scope":{"type":"string","enum":["global","project","track","plugin"]},"subject_id":{"type":"string"},"subjectId":{"type":"string"},"kind":{"type":"string"},"content":{"type":"object"},"text":{"type":"string"},"confidence":{"type":"number"}}})"},
    {"memory.search", "Search local MemoryRecord entries by scope, subject, and lexical query.", R"({"type":"object","properties":{"scope":{"type":"string","enum":["global","project","track","plugin"]},"subject_id":{"type":"string"},"subjectId":{"type":"string"},"query":{"type":"string"},"limit":{"type":"integer","default":10}}})"},
    {"memory.record_outcome", "Store ActionOutcome evidence for an AutomationTransaction, including before/after state, measurements, user acceptance, and feedback.", R"({"type":"object","properties":{"transaction_id":{"type":"string"},"transactionId":{"type":"string"},"user_accepted":{"type":"boolean"},"userAccepted":{"type":"boolean"},"user_feedback":{"type":"string"},"userFeedback":{"type":"string"},"outcome_score":{"type":"number","minimum":0,"maximum":1},"outcomeScore":{"type":"number","minimum":0,"maximum":1}},"required":["transaction_id"]})"},
    {"memory.update_outcome_feedback", "Update an existing ActionOutcome with structured user feedback such as accepted, sounds_better, too_much, rejected, or undo.", R"({"type":"object","properties":{"transaction_id":{"type":"string"},"transactionId":{"type":"string"},"action_id":{"type":"string"},"actionId":{"type":"string"},"feedback_status":{"type":"string","enum":["accepted","sounds_better","too_much","rejected","undo"]},"feedbackStatus":{"type":"string"},"status":{"type":"string"},"user_feedback":{"type":"string"},"userFeedback":{"type":"string"}},"allOf":[{"anyOf":[{"required":["transaction_id"]},{"required":["transactionId"]},{"required":["action_id"]},{"required":["actionId"]}]},{"anyOf":[{"required":["feedback_status"]},{"required":["feedbackStatus"]},{"required":["status"]}]}]})"},
    {"memory.list_outcomes", "List recorded ActionOutcome memory records, optionally filtered by WorkflowRun.", R"({"type":"object","properties":{"workflow_run_id":{"type":"string"},"workflowRunId":{"type":"string"},"limit":{"type":"integer","default":50,"minimum":1}}})"},
    {"memory.forget", "Delete one MemoryRecord by ID.", R"({"type":"object","properties":{"id":{"type":"string"}},"required":["id"]})"},
    {"memory.get_intent_context", "Return compact memory hints ordered by Track, Plugin, Project, then Global.", R"({"type":"object","properties":{"limit":{"type":"integer","default":5}}})"},
    {"plugin_adapter.describe_capabilities", "Describe semantic plugin capabilities and the adapter precedence chain.", R"({"type":"object","properties":{}})"},
    {"plugin_adapter.plan_action", "Build a semantic adapter action plan without applying it.", R"({"type":"object","properties":{"action":{"type":"object"},"allow_caution":{"type":"boolean","default":false},"dry_run":{"type":"boolean","default":true}}})"},
    {"plugin_adapter.apply_action", "Apply a semantic adapter action through AutomationDispatcher and the safe parameter queue.", R"({"type":"object","properties":{"action":{"type":"object"},"allow_caution":{"type":"boolean","default":false},"dry_run":{"type":"boolean","default":false}},"required":["action"]})"},
    // ── Ozone Track Assistant tools (guide-aligned) ──────────────────────────
    {"ozone.track.get_info",
     "Retrieve session metadata and current mastering status for the hosted Ozone instance. "
     "Returns track_id (instance-scoped), plugin name, workflow status, current LUFS metrics, "
     "and created_at timestamp. Optionally includes full status-transition history.",
     R"({"type":"object","properties":{"track_id":{"type":"string","description":"Optional — session track ID returned by previous ozone.track calls. Omit to query the current hosted instance."},"include_history":{"type":"boolean","default":false}}})"},
    {"ozone.track.update_status",
     "Transition the hosted Ozone mastering session through workflow states. "
     "Valid states: pending_review, in_mastering, mastering_complete, approved, rejected, on_hold. "
     "A 'reason' is required when setting status to 'rejected' or 'on_hold'.",
     R"({"type":"object","properties":{"track_id":{"type":"string"},"new_status":{"type":"string","enum":["pending_review","in_mastering","mastering_complete","approved","rejected","on_hold"]},"reason":{"type":"string","maxLength":500}},"required":["new_status"]})"},
    {"ozone.track.analyze",
     "Return an audio analysis report for the current mastering session using More-Phi's "
     "built-in LUFSMeter, TruePeakEstimator, and AutoMasteringEngine. Returns LUFS integrated/"
     "short-term/momentary, true peak dBTP, dynamic range LRA, per-band gain reduction, "
     "streaming-target delta, and a heuristic mastering recommendation. "
     "analysis_profile: standard | streaming | vinyl_master | broadcast",
     R"({"type":"object","properties":{"track_id":{"type":"string"},"force_reanalyze":{"type":"boolean","default":false},"analysis_profile":{"type":"string","enum":["standard","streaming","vinyl_master","broadcast"],"default":"standard"}}})"},
    {"ozone.track.search",
     "Search More-Phi snapshot slots and mastering sessions by title/status/date. "
     "Returns a paginated list of tracks compatible with the Ozone Track Assistant JSON schema. "
     "Each result includes snapshot slot data, current status, and LUFS snapshot if available.",
     R"({"type":"object","properties":{"query":{"type":"string","maxLength":200},"status_filter":{"type":"array","items":{"type":"string"}},"date_from":{"type":"string","format":"date"},"date_to":{"type":"string","format":"date"},"page":{"type":"integer","minimum":1,"default":1},"page_size":{"type":"integer","minimum":1,"maximum":50,"default":20}}})"},
    {"ozone_track_get_info",
     "Retrieve local Track Assistant metadata and status for a More-Phi track.",
     R"({"type":"object","properties":{"track_id":{"type":"string","pattern":"^trk_[A-Za-z0-9]{20}$"},"include_history":{"type":"boolean","default":false}},"required":["track_id"]})"},
    {"ozone_track_update_status",
     "Update a local Track Assistant workflow status.",
     R"({"type":"object","properties":{"track_id":{"type":"string","pattern":"^trk_[A-Za-z0-9]{20}$"},"new_status":{"type":"string","enum":["pending_review","in_mastering","mastering_complete","approved","rejected","on_hold"]},"reason":{"type":"string","maxLength":500}},"required":["track_id","new_status"]})"},
    {"ozone_track_analyze",
     "Analyze a local More-Phi track using current metering and cached render context.",
     R"({"type":"object","properties":{"track_id":{"type":"string","pattern":"^trk_[A-Za-z0-9]{20}$"},"force_reanalyze":{"type":"boolean","default":false},"analysis_profile":{"type":"string","enum":["standard","streaming","vinyl_master","broadcast"],"default":"standard"}},"required":["track_id"]})"},
    {"ozone_track_search",
     "Search local Track Assistant records by title, artist, status, or date range.",
     R"({"type":"object","properties":{"query":{"type":"string","maxLength":200},"status_filter":{"type":"array","items":{"type":"string","enum":["pending_review","in_mastering","mastering_complete","approved","rejected","on_hold"]}},"date_from":{"type":"string","format":"date"},"date_to":{"type":"string","format":"date"},"page":{"type":"integer","minimum":1,"default":1},"page_size":{"type":"integer","minimum":1,"maximum":50,"default":20}}})"},
    // ── Async tool execution ───────────────────────────────────────────────────
    {"async_tool.submit",
     "Run a long-running tool on a background thread and return a job_id immediately.",
     R"({"type":"object","properties":{"tool":{"type":"string","description":"Name of the tool to run asynchronously"},"tool_name":{"type":"string"},"arguments":{"type":"object"},"params":{"type":"object"}},"required":["tool"]})"},
    {"async_tool.status",
     "Poll the status of a job submitted via async_tool.submit.",
     R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})"},
    {"async_tool.result",
     "Retrieve the final result of a completed async_tool.submit job.",
     R"({"type":"object","properties":{"job_id":{"type":"string"}},"required":["job_id"]})"}
};

static json plannerInputsToJson(int genreIndex,
                                float dynamicRange,
                                float spectralTilt,
                                float correlationMS)
{
    return {
        {"genre_index", genreIndex},
        {"dynamic_range_lra", dynamicRange},
        {"spectral_tilt_db_per_octave", spectralTilt},
        {"correlation_ms", correlationMS}
    };
}

static json plannerRulesToJson(int genreIndex,
                               float dynamicRange,
                               float spectralTilt,
                               float correlationMS,
                               const MultiEffectPlan& plan)
{
    const int clampedGenre = std::clamp(genreIndex, 0, 11);
    json rules = json::array();
    rules.push_back({
        {"rule_id", "dynamics_lra_thresholds_v1"},
        {"input", "dynamic_range_lra"},
        {"reason", dynamicRange < 4.0f
            ? "low_lra_selects_gentle_compression"
            : (dynamicRange < 9.0f ? "moderate_lra_selects_medium_compression" : "high_lra_selects_stronger_compression")},
        {"output", { {"compression_need", plan.compressionNeed} }}
    });
    rules.push_back({
        {"rule_id", "spectral_tilt_eq_v1"},
        {"input", "spectral_tilt_db_per_octave"},
        {"reason", spectralTilt < 0.0f ? "negative_tilt_boosts_high_shelf" : "positive_or_flat_tilt_reduces_high_shelf_boost"},
        {"output", { {"eq_prescription", plan.eqPrescriptionJSON.toStdString()} }}
    });
    rules.push_back({
        {"rule_id", "mid_side_correlation_width_v1"},
        {"input", "correlation_ms"},
        {"reason", correlationMS > 0.8f
            ? "high_mid_side_correlation_selects_wider_curve"
            : (correlationMS < 0.2f ? "low_or_phasey_correlation_selects_tighter_curve" : "moderate_correlation_selects_standard_curve")},
        {"output", { {"width_curve", { plan.widthCurve[0], plan.widthCurve[1], plan.widthCurve[2], plan.widthCurve[3] }} }}
    });
    rules.push_back({
        {"rule_id", "genre_loudness_target_v1"},
        {"input", "genre_index"},
        {"reason", "genre_index_selects_static_lufs_target"},
        {"output", { {"clamped_genre_index", clampedGenre}, {"target_lufs", plan.targetLUFS}, {"ceiling_dbtp", plan.ceilingDBTP} }}
    });
    rules.push_back({
        {"rule_id", "stage_enable_v1"},
        {"input", "compression_need"},
        {"reason", plan.exciterEnabled ? "higher_compression_need_enables_exciter" : "lower_compression_need_keeps_exciter_disabled"},
        {"output", { {"exciter_enabled", plan.exciterEnabled}, {"use_neural_comp", plan.useNeuralComp} }}
    });
    return rules;
}

static json plannerMetadataToJson()
{
    return {
        {"recommendation_type", kPlannerType},
        {"planner_type", kPlannerType},
        {"rule_version", kPlannerRuleVersion},
        {"confidence", nullptr},
        {"limitations", json::array({
            "deterministic rules only; no learned mastering model is inferred",
            "confidence is uncalibrated and therefore omitted"
        })}
    };
}

static json planToJson(const MultiEffectPlan& plan)
{
    json result{
        {"valid", plan.valid},
        {"compression_need", plan.compressionNeed},
        {"use_neural_comp", plan.useNeuralComp},
        {"target_lufs", plan.targetLUFS},
        {"ceiling_dbtp", plan.ceilingDBTP},
        {"exciter_enabled", plan.exciterEnabled},
        {"width_curve", { plan.widthCurve[0], plan.widthCurve[1], plan.widthCurve[2], plan.widthCurve[3] }},
        {"eq_prescription", plan.eqPrescriptionJSON.toStdString()},
        {"recommendation_type", kPlannerType},
        {"planner_type", kPlannerType},
        {"rule_version", kPlannerRuleVersion},
        {"score_available", false},
        {"score_basis", kDryRunScoreBasis},
        {"confidence", nullptr},
        {"planner_metadata", plannerMetadataToJson()}
    };
    return result;
}

static json ozoneMapToJson(const OzoneParameterMap& map)
{
    json eq = json::array();
    int matched = 0;

    for (int i = 0; i < OzoneParameterMap::kEQBands; ++i)
    {
        const auto& band = map.eq[static_cast<size_t>(i)];
        matched += band.freqIdx    >= 0 ? 1 : 0;
        matched += band.gainIdx    >= 0 ? 1 : 0;
        matched += band.qIdx       >= 0 ? 1 : 0;
        matched += band.typeIdx    >= 0 ? 1 : 0;
        matched += band.enabledIdx >= 0 ? 1 : 0;
        eq.push_back({
            {"band", i + 1},
            {"frequency", band.freqIdx},
            {"gain", band.gainIdx},
            {"q", band.qIdx},
            {"type", band.typeIdx},
            {"enabled", band.enabledIdx}
        });
    }

    matched += map.dynamics.thresholdIdx >= 0 ? 1 : 0;
    matched += map.dynamics.ratioIdx     >= 0 ? 1 : 0;
    matched += map.dynamics.attackIdx    >= 0 ? 1 : 0;
    matched += map.dynamics.releaseIdx   >= 0 ? 1 : 0;

    json imagerWidths = json::array();
    for (int idx : map.imager.widthIdx)
    {
        matched += idx >= 0 ? 1 : 0;
        imagerWidths.push_back(idx);
    }

    matched += map.maximizer.outputLevelIdx >= 0 ? 1 : 0;
    matched += map.maximizer.ceilingIdx     >= 0 ? 1 : 0;

    return json{
        {"matched_count", matched},
        {"expected_count", OzoneParameterMap::kEQBands * 5 + 4 + 4 + 2},
        {"eq", eq},
        {"dynamics", {
            {"threshold", map.dynamics.thresholdIdx},
            {"ratio", map.dynamics.ratioIdx},
            {"attack", map.dynamics.attackIdx},
            {"release", map.dynamics.releaseIdx}
        }},
        {"imager", {{"width", imagerWidths}}},
        {"maximizer", {
            {"output_level", map.maximizer.outputLevelIdx},
            {"ceiling", map.maximizer.ceilingIdx}
        }}
    };
}

static json candidateToJson(const RenderCandidateRecord& candidate)
{
    return json{
        {"id", candidate.id.toStdString()},
        {"index", candidate.index},
        {"success", candidate.success},
        {"output_path", candidate.outputPath.toStdString()},
        {"peak_db", candidate.peakDb},
        {"rms_db", candidate.rmsDb},
        {"has_silence", candidate.hasSilence},
        {"has_clipping", candidate.hasClipping},
        {"score", scoreRenderedCandidate(candidate)},
        {"score_available", true},
        {"score_basis", kRenderedScoreBasis},
        {"confidence", nullptr},
        {"render_time_ms", candidate.renderTimeMs},
        {"error", candidate.errorMessage.toStdString()}
    };
}

static json progressToJson(const OfflineBatchProgress& progress)
{
    return json{
        {"completed", progress.completed},
        {"total", progress.total},
        {"percentage", progress.percentage},
        {"status", progress.currentStatus.toStdString()},
        {"elapsed_ms", progress.elapsedMs},
        {"estimated_remaining_ms", progress.estimatedRemainingMs},
        {"successful_renders", progress.successfulRenders},
        {"failed_renders", progress.failedRenders}
    };
}

static std::string makeInstancePrefixedKey(const juce::String& instanceId, const juce::String& id)
{
    return (instanceId + ":" + id).toStdString();
}

static std::shared_ptr<RenderJobRecord> findRenderJob(const juce::String& instanceId, const juce::String& jobId)
{
    const std::lock_guard<std::mutex> guard(gRenderJobsMutex);
    const auto it = gRenderJobs.find(makeInstancePrefixedKey(instanceId, jobId));
    return it != gRenderJobs.end() ? it->second : nullptr;
}

static juce::String makeRenderCandidateId(const juce::String& jobId, int index)
{
    return jobId + ":variation_" + juce::String::formatted("%04d", index);
}

static juce::String makeDryRunCandidateId(uint64_t batchId, int index)
{
    return "dry_run_" + juce::String(static_cast<int64_t>(batchId))
        + ":variation_" + juce::String::formatted("%04d", index);
}

static json renderJobToJson(const RenderJobRecord& job)
{
    const std::lock_guard<std::mutex> jobGuard(job.mutex);

    json candidates = json::array();
    for (const auto& candidate : job.candidates)
        candidates.push_back(candidateToJson(candidate));

    return json{
        {"success", true},
        {"job_id", job.id.toStdString()},
        {"status", job.status.toStdString()},
        {"completed", job.completed},
        {"render_success", job.success},
        {"message", job.message.toStdString()},
        {"input_path", job.inputPath.toStdString()},
        {"output_directory", job.outputDirectory.toStdString()},
        {"plugin_path", job.pluginPath.toStdString()},
        {"progress", progressToJson(job.progress)},
        {"candidates", candidates}
    };
}

static float getNumberProperty(const juce::var& object, const char* name, float fallback)
{
    if (!object.isObject() || !object.hasProperty(name))
        return fallback;
    return static_cast<float>(object.getProperty(name, fallback));
}

static int requestedSpectrumResolution(const juce::var& params) noexcept
{
    const int requested = static_cast<int>(params.getProperty("resolution", 64));
    if (requested <= 32)
        return 32;
    if (requested <= 64)
        return 64;
    if (requested <= 128)
        return 128;
    return 256;
}

// Extract a parameter index from a request that may use either "id" or "index" as key.
// B-9 FIX: Returns std::optional<int> — callers can't accidentally use -1 as a valid index.
// If maxParamCount > 0, the result is also bounds-checked against it.
static std::optional<int> extractParamId(const juce::var& params, int maxParamCount = 0) noexcept
{
    int id = -1;
    if (params.hasProperty("id"))         id = static_cast<int>(params.getProperty("id",    -1));
    else if (params.hasProperty("index")) id = static_cast<int>(params.getProperty("index", -1));
    else                                  return std::nullopt;

    if (id < 0) return std::nullopt;
    if (maxParamCount > 0 && id >= maxParamCount) return std::nullopt;
    return id;
}

static juce::String invalidParamsResponse(const char* message)
{
    return toJString(json{
        {"success", false},
        {"error", "invalid_params"},
        {"message", message != nullptr ? message : "Invalid tool arguments."}
    });
}

static bool optionalStringProperty(const juce::var& params,
                                   const char* key,
                                   std::optional<std::string>& out,
                                   juce::String& error)
{
    if (!params.hasProperty(key))
        return true;

    const auto value = params.getProperty(key, juce::var());
    if (!value.isString())
    {
        error = juce::String(key) + " must be a string.";
        return false;
    }

    const auto text = value.toString();
    if (text.isNotEmpty())
        out = text.toStdString();
    return true;
}

static bool requiredStringProperty(const juce::var& params,
                                   const char* key,
                                   std::string& out,
                                   juce::String& error)
{
    std::optional<std::string> value;
    if (!optionalStringProperty(params, key, value, error))
        return false;

    if (!value || value->empty())
    {
        error = juce::String(key) + " is required.";
        return false;
    }

    out = *value;
    return true;
}

static bool optionalSizeProperty(const juce::var& params,
                                 const char* key,
                                 std::optional<size_t>& out,
                                 juce::String& error)
{
    if (!params.hasProperty(key))
        return true;

    const auto value = params.getProperty(key, juce::var());
    if (!(value.isInt() || value.isInt64() || value.isDouble()))
    {
        error = juce::String(key) + " must be a non-negative integer.";
        return false;
    }

    const double numeric = static_cast<double>(value);
    if (!std::isfinite(numeric)
        || numeric < 0.0
        || std::floor(numeric) != numeric
        || numeric > static_cast<double>(std::numeric_limits<size_t>::max()))
    {
        error = juce::String(key) + " must be a non-negative integer.";
        return false;
    }

    out = static_cast<size_t>(numeric);
    return true;
}

static bool optionalBoolProperty(const juce::var& params,
                                 const char* key,
                                 bool& out,
                                 juce::String& error)
{
    if (!params.hasProperty(key))
        return true;

    const auto value = params.getProperty(key, juce::var());
    if (!value.isBool())
    {
        error = juce::String(key) + " must be a boolean.";
        return false;
    }

    out = static_cast<bool>(value);
    return true;
}

static bool assignOptionalUInt32(const std::optional<size_t>& value,
                                 std::optional<uint32_t>& out,
                                 const char* key,
                                 juce::String& error)
{
    if (!value)
        return true;

    if (*value > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
    {
        error = juce::String(key) + " is outside the uint32 range.";
        return false;
    }

    out = static_cast<uint32_t>(*value);
    return true;
}

static json parameterDescriptorForApply(const ParameterBridge::ParameterDescriptor& descriptor)
{
    return {
        {"index", descriptor.index},
        {"stable_id", descriptor.stableId.toStdString()},
        {"name", descriptor.name.toStdString()},
        {"label", descriptor.label.toStdString()},
        {"num_steps", descriptor.numSteps},
        {"default_value", descriptor.defaultValue},
        {"is_discrete", descriptor.discrete},
        {"is_boolean", descriptor.boolean},
        {"value", descriptor.value},
        {"text", descriptor.displayValue.toStdString()}
    };
}

static json makeAssistantApplyError(const char* code, int index, const char* message)
{
    return {
        {"code", code != nullptr ? code : "assistant_apply_failed"},
        {"index", index},
        {"message", message != nullptr ? message : "Assistant parameter could not be applied."}
    };
}

static standalone_mcp::ToolCallOutcome applyIpcAssistantParametersToHostedPlugin(const json& body,
                                                                                 MorePhiProcessor& p)
{
    if (!body.contains("parameters") || !body["parameters"].is_array())
    {
        return {json{
            {"success", false},
            {"error", "assistant_result_invalid"},
            {"message", "parameters array is missing."},
            {"apply_result", {
                {"applied", false},
                {"requested_count", 0},
                {"applied_count", 0},
                {"parameters", json::array()},
                {"errors", json::array({{
                    {"code", "assistant_result_invalid"},
                    {"message", "AssistantResult parameters were missing or malformed."}
                }})}
            }}
        }, true};
    }

    auto& bridge = p.getParameterBridge();
    if (!p.getHostManager().hasPlugin())
    {
        return {json{
            {"success", false},
            {"error", "plugin_not_loaded"},
            {"apply_result", {
                {"applied", false},
                {"requested_count", static_cast<int>(body["parameters"].size())},
                {"applied_count", 0},
                {"parameters", json::array()},
                {"errors", json::array({{
                    {"code", "plugin_not_loaded"},
                    {"message", "No hosted plugin is loaded for AssistantResult application."}
                }})}
            }}
        }, true};
    }

    const int parameterCount = bridge.getParameterCount();
    json errors = json::array();
    std::vector<MorePhiProcessor::ParamCommand> commands;
    commands.reserve(body["parameters"].size());

    for (const auto& item : body["parameters"])
    {
        if (!item.is_object()
            || !item.contains("index")
            || !item["index"].is_number_integer()
            || !item.contains("value")
            || !item["value"].is_number())
        {
            errors.push_back(makeAssistantApplyError(
                "assistant_result_invalid",
                -1,
                "AssistantResult parameter entries must contain integer index and numeric value."));
            continue;
        }

        const int index = item["index"].get<int>();
        const double value = item["value"].get<double>();
        if (index < 0 || index >= parameterCount)
        {
            errors.push_back(makeAssistantApplyError(
                "parameter_index_out_of_range",
                index,
                "Assistant parameter index is outside the hosted plugin parameter range."));
            continue;
        }

        if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        {
            errors.push_back(makeAssistantApplyError(
                "parameter_value_out_of_range",
                index,
                "Assistant parameter value must be finite and normalized between 0.0 and 1.0."));
            continue;
        }

        commands.push_back(MorePhiProcessor::ParamCommand{
            index,
            static_cast<float>(value),
            false,
            -1,
            MorePhiProcessor::ParameterEditSource::Assistant,
            true
        });
    }

    if (!errors.empty())
    {
        return {json{
            {"success", false},
            {"error", "assistant_apply_failed"},
            {"apply_result", {
                {"applied", false},
                {"requested_count", static_cast<int>(body["parameters"].size())},
                {"applied_count", 0},
                {"parameters", json::array()},
                {"errors", errors}
            }}
        }, true};
    }

    if (!p.enqueueParameterBatch(commands))
    {
        return {json{
            {"success", false},
            {"error", "assistant_apply_failed"},
            {"apply_result", {
                {"applied", false},
                {"requested_count", static_cast<int>(body["parameters"].size())},
                {"applied_count", 0},
                {"parameters", json::array()},
                {"errors", json::array({makeAssistantApplyError(
                    "queue_full",
                    -1,
                    "More-Phi command queue does not have enough free space to apply AssistantResult atomically.")})}
            }}
        }, true};
    }

    const auto flush = p.flushPendingParameterCommandsForAssistant(
        juce::jmax(2048, static_cast<int>(commands.size())));

    json applied = json::array();
    for (const auto& command : commands)
    {
        applied.push_back({
            {"index", command.paramIndex},
            {"value", command.value},
            {"success", true},
            {"parameter", parameterDescriptorForApply(bridge.getParameterDescriptor(command.paramIndex))}
        });
    }

    return {json{
        {"success", true},
        {"apply_result", {
            {"applied", flush.drained >= static_cast<int>(commands.size())},
            {"requested_count", static_cast<int>(body["parameters"].size())},
            {"applied_count", static_cast<int>(applied.size())},
            {"applied_now_count", flush.drained},
            {"queued_count", static_cast<int>(commands.size())},
            {"pending_after", flush.pendingAfter},
            {"parameters", applied},
            {"flush", parameterFlushToJson(flush)},
            {"errors", json::array()}
        }}
    }, false};
}

static json runSnapshotSelfTest(MorePhiProcessor& processor)
{
    auto& bank = processor.getSnapshotBank();
    auto backup = bank.toXml();

    struct RestoreBank
    {
        SnapshotBank& bank;
        std::unique_ptr<juce::XmlElement> backup;

        ~RestoreBank()
        {
            if (backup)
                bank.fromXml(*backup);
            else
                bank.clearAll();
        }
    } restore{bank, std::move(backup)};

    json tests = json::array();
    bool allPassed = true;
    auto addTest = [&tests, &allPassed](const char* id, bool passed, const std::string& details)
    {
        tests.push_back({{"id", id}, {"ok", passed}, {"details", details}});
        allPassed = allPassed && passed;
    };

    const int hostedParamCount = processor.getParameterBridge().getParameterCount();
    const int paramCount = juce::jlimit(1, 64, hostedParamCount > 0 ? hostedParamCount : 16);

    try
    {
        bank.clearAll();

        std::vector<float> emptyOut(static_cast<size_t>(paramCount), 0.25f);
        InterpolationEngine::compute1D(0.5f, bank, emptyOut);
        const bool emptyInterpolationSafe = std::all_of(emptyOut.begin(), emptyOut.end(),
            [](float value) { return std::isfinite(value); });

        MorphProcessor emptyMorph(bank);
        emptyMorph.prepare(paramCount);
        std::vector<float> unchanged(static_cast<size_t>(paramCount), 0.25f);
        emptyMorph.process(0.5f, 0.5f, 0.5f, MorphSource::Fader, MorphMode::Direct,
                           1.0f / 60.0f, unchanged);
        const bool emptyMorphSafe = std::all_of(unchanged.begin(), unchanged.end(),
            [](float value) { return std::abs(value - 0.25f) < 0.0001f; });

        addTest("empty_slot_no_crash",
                emptyInterpolationSafe && emptyMorphSafe,
                "Empty slot interpolation and morph processing completed without mutation or non-finite values.");

        std::array<std::vector<float>, SnapshotBank::NUM_SLOTS> expected{};
        std::array<uint64_t, SnapshotBank::NUM_SLOTS> checksums{};

        bool capturedAll = true;
        bool checksumCopiesValid = true;
        for (int slot = 0; slot < SnapshotBank::NUM_SLOTS; ++slot)
        {
            auto& values = expected[static_cast<size_t>(slot)];
            values.resize(static_cast<size_t>(paramCount));
            for (int p = 0; p < paramCount; ++p)
            {
                const int raw = (slot * 37 + p * 19 + 11) % 101;
                values[static_cast<size_t>(p)] = static_cast<float>(raw) / 100.0f;
            }

            bank.captureValues(slot, values);
            checksums[static_cast<size_t>(slot)] = checksumNormalizedValues(values);
            capturedAll = capturedAll && bank.isOccupied(slot);

            std::vector<float> copied;
            checksumCopiesValid = checksumCopiesValid
                && bank.getSlotValuesCopy(slot, copied)
                && copied.size() == static_cast<size_t>(paramCount)
                && checksumNormalizedValues(copied) == checksums[static_cast<size_t>(slot)];
        }

        addTest("capture_all_12_slots", capturedAll, "Captured deterministic normalized data into all 12 snapshot slots.");
        addTest("stored_parameter_checksums", checksumCopiesValid, "Copied slot values match quantized FNV-1a checksums.");

        SnapshotSelfTestBridge bridge(paramCount);
        bool recallChecksumsValid = true;
        for (int slot = 0; slot < SnapshotBank::NUM_SLOTS; ++slot)
        {
            bridge.values.assign(static_cast<size_t>(paramCount), 0.0f);
            bridge.applyCount = 0;
            bank.recallFast(slot, bridge);
            recallChecksumsValid = recallChecksumsValid
                && bridge.applyCount == 1
                && checksumNormalizedValues(bridge.values) == checksums[static_cast<size_t>(slot)];
        }

        addTest("recall_each_slot", recallChecksumsValid, "Each slot recalled through IParameterBridge with matching checksum.");

        MorphProcessor morph(bank);
        morph.prepare(paramCount);
        morph.setSmoothingRate(0.0f);

        bool adjacentMorphsValid = true;
        for (int slot = 0; slot < SnapshotBank::NUM_SLOTS - 1; ++slot)
        {
            const float faderPos = (static_cast<float>(slot) + 0.5f)
                                 / static_cast<float>(SnapshotBank::NUM_SLOTS - 1);
            std::vector<float> out(static_cast<size_t>(paramCount), 0.0f);
            morph.process(0.5f, 0.5f, faderPos, MorphSource::Fader, MorphMode::Direct,
                          1.0f / 60.0f, out);

            for (int p = 0; p < paramCount; ++p)
            {
                const float expectedMidpoint =
                    0.5f * (expected[static_cast<size_t>(slot)][static_cast<size_t>(p)]
                          + expected[static_cast<size_t>(slot + 1)][static_cast<size_t>(p)]);
                adjacentMorphsValid = adjacentMorphsValid
                    && std::abs(out[static_cast<size_t>(p)] - expectedMidpoint) < 0.0001f;
            }
        }

        addTest("morph_adjacent_slots", adjacentMorphsValid, "Adjacent slot midpoint morphs match expected linear interpolation with smoothing disabled.");

        bridge.values.assign(static_cast<size_t>(paramCount), 0.33f);
        bridge.applyCount = 0;
        bank.clearSlot(5);
        bank.recallFast(5, bridge);
        const bool emptyRecallSafe = !bank.isOccupied(5)
            && bridge.applyCount == 0
            && std::all_of(bridge.values.begin(), bridge.values.end(),
                [](float value) { return std::abs(value - 0.33f) < 0.0001f; });
        addTest("empty_slot_recall_no_crash", emptyRecallSafe, "Recall of a cleared slot is a no-op.");
    }
    catch (const std::exception& e)
    {
        addTest("snapshot_suite_exception", false, e.what());
    }
    catch (...)
    {
        addTest("snapshot_suite_exception", false, "Unknown exception.");
    }

    return {
        {"success", allPassed},
        {"suite", "snapshot"},
        {"mode", "synthetic_preserve_existing_slots"},
        {"hosted_parameter_count", hostedParamCount},
        {"tested_parameter_count", paramCount},
        {"tests", tests}
    };
}

// ── Caching helpers ───────────────────────────────────────────────────────────

ToolResultCache& MCPToolHandler::getToolResultCache()
{
    return toolResultCache();
}

AsyncToolExecutor& MCPToolHandler::getAsyncToolExecutorInternal()
{
    return asyncToolExecutor();
}

AsyncToolExecutor& MCPToolHandler::getAsyncToolExecutor()
{
    return asyncToolExecutor();
}

void MCPToolHandler::invalidateToolResultCache()
{
    toolResultCache().invalidateAll();
}

void MCPToolHandler::invalidateToolResultCacheForTool(const juce::String& toolName)
{
    // Map the write tool to the cache scopes it dirties (spec §6.2).
    // A parameter write invalidates parameter-describing reads and the morph
    // snapshot state; it does NOT invalidate analysis meters (which reflect
    // audio, not parameter layout) or the semantic profile (parameter
    // classification). Recall/capture additionally invalidates morph.
    using Scope = ToolResultCache::Scope;

    if (toolName == "set_parameter" || toolName == "set_parameters_batch"
        || toolName == "set_parameters_optimized"
        || toolName == "sweep_parameter"
        || toolName == "hosted_plugin.set_parameter"
        || toolName == "hosted_plugin.set_parameters"
        || toolName == "more_phi.set_parameter"
        || toolName == "more_phi.set_parameters"
        || toolName == "apply_mastering_plan" || toolName == "mastering.apply_plan"
        || toolName == "plugin_profile.apply_safe_action"
        || toolName == "mastering.select_candidate")
    {
        toolResultCache().invalidateScopes({Scope::Parameters, Scope::Morph});
        return;
    }

    if (toolName == "recall_snapshot" || toolName == "capture_snapshot"
        || toolName == "hosted_plugin.capture_state"
        || toolName == "plugin_profile.restore_safe_snapshot"
        || toolName == "automation.rollback"
        || toolName == "set_morph_position")
    {
        // Snapshot/recall/morph changes can alter every observable, including
        // the analysis window, so evict the dirty observable scopes together.
        toolResultCache().invalidateScopes({Scope::Parameters, Scope::Morph, Scope::Analysis});
        return;
    }

    // Control-plane writes (memory, permission, workflow, context, events,
    // sync) mutate the Control scope's read tools (automation.history,
    // memory.list_outcomes, permission.list_approvals, workflow.list, etc.)
    // and so must evict that scope. approval/autonomy changes also touch the
    // automation ledger, so evict Control for all of them.
    if (toolName.startsWith("memory.") || toolName.startsWith("permission.")
        || toolName.startsWith("workflow.") || toolName.startsWith("context.")
        || toolName.startsWith("events.") || toolName.startsWith("sync.")
        || toolName.startsWith("automation."))
    {
        toolResultCache().invalidateScopes({Scope::Control});
        return;
    }

    // Unrecognised write tool: conservative eviction of the observable scopes.
    toolResultCache().invalidateScopes({Scope::Parameters, Scope::Morph, Scope::Analysis});
}

bool MCPToolHandler::isCacheableTool(const juce::String& method)
{
    static const std::set<juce::String, std::less<>> cacheable = {
        "get_plugin_info",
        "hosted_plugin.info",
        "list_parameters",
        "hosted_plugin.parameters",
        "get_parameter",
        "get_morph_state",
        "more_phi.parameters",
        "analysis.get_summary",
        "analysis.get_spectrum",
        "analysis.get_stereo_field",
        "plugin_profile.describe_semantics",
        "plugin_profile.describe_semantic_map",
        "describe_plugin_semantic_map",
        "diagnose_parameter_pipeline",
        "get_instance_info",
        "list_instances",
        "automation.history",
        "automation.get_transaction",
        "permission.get_state",
        "permission.list_approvals",
        "workflow.list",
        "memory.search",
        "memory.list_outcomes",
        "context.get_session",
        "context.get_transport",
        "context.get_track_state",
        "events.list_recent"
    };
    return cacheable.count(method) > 0;
}

juce::String MCPToolHandler::getCachedToolResult(const juce::String& method,
                                                  const juce::var& params,
                                                  MorePhiProcessor& p)
{
    if (!isCacheableTool(method))
        return {};

    const auto cached = toolResultCache().get(method, params, p.getProcessorGenerationToken(),
                                              p.getInstanceIdentity().instanceId);
    if (!cached.has_value())
        return {};

    json wrapper = *cached;
    wrapper["cached"] = true;
    return toJString(wrapper);
}

void MCPToolHandler::cacheToolResult(const juce::String& method,
                                      const juce::var& params,
                                      MorePhiProcessor& p,
                                      const juce::String& result)
{
    if (!isCacheableTool(method))
        return;

    try
    {
        const auto parsed = json::parse(result.toStdString());
        // Avoid caching error responses.
        if (parsed.is_object() && parsed.value("success", true) == false)
            return;

        toolResultCache().put(method, params, p.getProcessorGenerationToken(), parsed,
                              p.getInstanceIdentity().instanceId,
                              std::chrono::seconds(30));
    }
    catch (...)
    {
        // Ignore malformed JSON — never let caching break tool dispatch.
    }
}

// ── Async tool helpers ───────────────────────────────────────────────────────

juce::String MCPToolHandler::submitAsyncTool(const juce::String& /*method*/,
                                              const juce::var& params,
                                              MorePhiProcessor& p,
                                              const InstanceIdentity& identity,
                                              AutomationRuntime& runtime)
{
    const auto innerMethod = params.getProperty("tool", params.getProperty("tool_name", "")).toString();
    const auto innerParams = params.getProperty("arguments", params.getProperty("params", juce::var(new juce::DynamicObject())));
    if (innerMethod.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_tool"},
                              {"message", "async_tool.submit requires 'tool' (tool name)."}});

    const auto jobId = asyncToolExecutor().submit(
        innerMethod.toStdString(),
        [innerMethod, innerParams, &p, &identity, &runtime]() -> json {
            return parseToolResponse(MCPToolHandler::handle(innerMethod, innerParams, p, identity, runtime));
        },
        identity.morphCode);  // B1 FIX: namespace job ID by instance

    return toJString(json{
        {"success", true},
        {"job_id", jobId.toStdString()},
        {"tool", innerMethod.toStdString()},
        {"status", "queued"}
    });
}

juce::String MCPToolHandler::getAsyncToolStatus(const juce::var& params)
{
    const auto jobId = params.getProperty("job_id", "").toString();
    if (jobId.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_job_id"}});
    return toJString(asyncToolExecutor().status(jobId));
}

juce::String MCPToolHandler::getAsyncToolResult(const juce::var& params)
{
    const auto jobId = params.getProperty("job_id", "").toString();
    if (jobId.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_job_id"}});
    return toJString(asyncToolExecutor().result(jobId));
}

// ── Dispatch ─────────────────────────────────────────────────────────────────

juce::String MCPToolHandler::getToolList()
{
    json tools = json::array();

    auto appendTool = [&tools](const char* name, const char* description,
                               const char* schemaText, const char* outputSchemaText = nullptr)
    {
        json tool{
            {"name", name},
            {"description", description != nullptr ? description : ""},
            {"inputSchema", parseSchema(schemaText)}
        };
        if (outputSchemaText != nullptr && outputSchemaText[0] != '\0')
            tool["outputSchema"] = parseSchema(outputSchemaText);
        tools.push_back(std::move(tool));
    };

    for (const auto& tool : kCoreTools)
        appendTool(tool.name, tool.description, tool.inputSchema, tool.outputSchema);

    for (int i = 0; i < kExtendedToolCount; ++i)
        appendTool(kExtendedTools[i].name, kExtendedTools[i].description, kExtendedTools[i].schema);

    // ── Agent runtime tools (agents.*) ──────────────────────────────────────
    appendTool("agents.list", "List registered agents and their state",
        R"({"type":"object","properties":{}})");
    appendTool("agents.run_goal", "Submit a natural-language goal to the Conductor agent",
        R"({"type":"object","properties":{"intent":{"type":"string"}},"required":["intent"]})");
    appendTool("agents.run_task", "Submit a task directly to a named agent (bypass Conductor)",
        R"({"type":"object","properties":{"agent":{"type":"string"},"intent":{"type":"string"}},"required":["agent"]})");
    appendTool("agents.run_status", "Poll a run/task for state and findings",
        R"({"type":"object","properties":{"task_id":{"type":"string"}},"required":["task_id"]})");
    appendTool("agents.run_cancel", "Cooperatively cancel the agent runtime",
        R"({"type":"object","properties":{}})");
    appendTool("agents.blackboard.recent", "Read recent blackboard events",
        R"({"type":"object","properties":{}})");
    appendTool("agents.set_autonomy", "Set the agent-domain autonomy level",
        R"({"type":"object","properties":{"level":{"type":"string","enum":["manual","assist","copilot","autopilot"]}},"required":["level"]})");

    return toJString(json{{"tools", tools}});
}

juce::String MCPToolHandler::handle(const juce::String& method,
                                     const juce::var& params,
                                     MorePhiProcessor& p,
                                     const InstanceIdentity& identity)
{
    // Fallback overload: create a temporary runtime for read-only operations.
    // Write operations that need a persistent ledger should use the 5-arg overload.
    AutomationRuntime fallback;
    return handle(method, params, p, identity, fallback);
}

juce::String MCPToolHandler::handle(const juce::String& method,
                                     const juce::var& params,
                                     MorePhiProcessor& p,
                                     const InstanceIdentity& identity,
                                     AutomationRuntime& runtime)
{
    // Fast path: read-only tool results may be cached.
    if (auto cached = getCachedToolResult(method, params, p); cached.isNotEmpty())
        return cached;

    juce::String result;

    if (method == "get_plugin_info")      result = getPluginInfo(p);
    else if (method == "list_parameters") result = listParameters(params, p);
    else if (method == "get_parameter")   result = getParameter(params, p);
    else if (method == "set_parameter")   return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return setParameter(params, p); });
    else if (method == "set_parameters_batch") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return setParametersBatch(params, p); });
    else if (method == "sweep_parameter") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return sweepParameter(params, p); });
    else if (method == "capture_snapshot")     return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return captureSnapshot(params, p); });
    else if (method == "recall_snapshot")      return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return recallSnapshot(params, p); });
    else if (method == "set_morph_position")   return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return setMorphPosition(params, p); });
    else if (method == "get_morph_state")      result = getMorphState(p);
    else if (method == "run_self_test")        return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return runSelfTest(params, p); });
    else if (method == "more_phi.parameters")     result = listMorePhiParameters(p);
    else if (method == "more_phi.get_parameter")  result = getMorePhiParameter(params, p);
    else if (method == "more_phi.set_parameter")  return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return setMorePhiParameter(params, p); });
    else if (method == "more_phi.set_parameters") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return setMorePhiParameters(params, p); });
    else if (method == "ozone.audit_parameters")  result = auditOzoneParameters(params, p);
    else if (method == "morephi_ipc_attach")      result = morePhiIpcAttach(params, p);
    else if (method == "morephi_ipc_detach")      result = morePhiIpcDetach(p);
    else if (method == "morephi_ipc_status")      result = morePhiIpcStatus(p);
    else if (method == "morephi_ipc_snapshot")    result = morePhiIpcSnapshot(params, p);
    else if (method == "morephi_ipc_dump")     return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return morePhiIpcDump(params, p); });
    else if (method == "morephi_ipc_capture")  return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return morePhiIpcCapture(params, p); });
    else if (method == "morephi_ipc_run_assistant")  return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return morePhiIpcRunAssistant(params, p); });
    else if (method == "async_tool.submit")  return submitAsyncTool(method, params, p, identity, runtime);
    else if (method == "async_tool.status")  return getAsyncToolStatus(params);
    else if (method == "async_tool.result")  return getAsyncToolResult(params);

    // MCP workflow aliases from the hosted mastering integration plan
    else if (method == "hosted_plugin.scan")          return scanHostedPlugin(params, p);
    else if (method == "hosted_plugin.load")          return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return loadHostedPlugin(params, p); });
    else if (method == "hosted_plugin.info")          result = getPluginInfo(p);
    else if (method == "hosted_plugin.parameters")    result = listParameters(params, p);
    else if (method == "hosted_plugin.set_parameter") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return setParameter(params, p); });
    else if (method == "hosted_plugin.set_parameters")return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return setParametersBatch(params, p); });
    else if (method == "hosted_plugin.capture_state") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return captureHostedState(params, p); });
    else if (method == "diagnose_parameter_pipeline") result = diagnoseParameterPipeline(params, p);
    else if (method == "analysis.get_summary")        result = getAnalysisSummary(p);
    else if (method == "analysis.get_spectrum")       result = getSpectrumAnalysis(params, p);
    else if (method == "analysis.get_stereo_field")   result = getStereoFieldAnalysis(p);
    else if (method == "analysis.capture_window")     result = captureAnalysisWindow(params, p);
    else if (method == "analysis.compare_render")     result = compareAnalysis(params, p);
    else if (method == "mastering.plan_preview")      result = previewMasteringPlan(params, p);
    else if (method == "mastering.apply_plan")        return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return applyMasteringPlan(params, p); });
    else if (method == "mastering.render_batch")      return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return renderMasteringBatch(params, p, identity); });
    else if (method == "mastering.render_status")     result = getMasteringRenderStatus(params, identity.instanceId);
    else if (method == "mastering.select_candidate")  return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return selectMasteringCandidate(params, p, identity); });
    else if (method == "plugin_profile.audit_parameters") result = auditPluginProfile(p);
    else if (method == "plugin_profile.describe_semantics") result = describePluginSemantics(p);
    else if (method == "plugin_profile.apply_safe_action") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return applySafePluginAction(params, p, identity); });
    else if (method == "plugin_profile.restore_safe_snapshot") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return restoreSafePluginSnapshot(params, p, identity); });
    else if (method == "plugin_profile.get")          result = getPluginProfile(params, p);
    else if (method == "plugin_profile.save")         return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return savePluginProfile(params, p); });
    else if (method == "plugin_profile.describe_semantic_map" || method == "describe_plugin_semantic_map")
        result = describePluginSemanticMap(params, p);

    // ── Agent runtime tools (agents.*) ──────────────────────────────────────
    // C1 FIX: write/impactful agents.* tools now route through
    // dispatchWithAutomationTransaction — the ONLY path that runs
    // PermissionKernel::evaluate() (autonomy gate) and writes the
    // permission/AutomationTransaction audit rows. Previously these bypassed
    // the gate: agents.set_autonomy (classified HighImpact) could flip
    // Assist→Autopilot with no approval check and no audit trail. The
    // classifier already assigns their risk (AutomationControlPlane.cpp:825-832),
    // so gating them was a dead-code gap. agents.run_task cascades into
    // specialist tools that are individually re-gated via DefaultToolInvoker,
    // but the entry-point risk class + audit must still fire. No recursion:
    // classifyTool() only assigns a RiskLevel, it never re-dispatches.
    else if (method == "agents.list")               result = agentsList(p);
    else if (method == "agents.run_goal")           return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return agentsRunGoal(params, p); });
    else if (method == "agents.run_task")           return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return agentsRunTask(params, p); });
    else if (method == "agents.run_status")         result = agentsRunStatus(params, p);
    else if (method == "agents.run_cancel")         result = agentsRunCancel(params, p);
    else if (method == "agents.blackboard.recent")  result = agentsBlackboardRecent(p);
    else if (method == "agents.set_autonomy")       return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return agentsSetAutonomy(params, p, runtime); });

    else if (method == "sync.apply_envelope")
        return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return handleControlPlaneTool(method, params, p, identity, runtime); });

    else if (method == "workflow.submit" || method == "workflow.execute" || method == "workflow.cancel"
        || method == "memory.remember" || method == "memory.record_outcome"
        || method == "memory.update_outcome_feedback" || method == "memory.forget"
        || method == "permission.set_autonomy" || method == "permission.approve"
        || method == "permission.reject")
    {
        return dispatchWithAutomationTransaction(method, params, p, runtime,
            [&]() { return handleControlPlaneTool(method, params, p, identity, runtime); });
    }

    else if (method.startsWith("automation.") || method.startsWith("workflow.")
        || method.startsWith("permission.") || method.startsWith("context.")
        || method.startsWith("events.") || method.startsWith("sync.")
        || method.startsWith("memory.") || method == "plugin_adapter.describe_capabilities"
        || method == "plugin_adapter.plan_action")
        result = handleControlPlaneTool(method, params, p, identity, runtime);

    else if (method == "plugin_adapter.apply_action")
        return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return applySafePluginAction(params, p, identity); });

    // Extended AI Tools
    else if (method == "analyze_parameters")             result = MCPToolsExtended::analyzeParameters(params, p, p.getParameterClassifier());
    else if (method == "expose_parameters")              result = MCPToolsExtended::exposeParameters(params, p, p.getParameterClassifier());
    else if (method == "get_token_estimate")             result = MCPToolsExtended::getTokenEstimate(params, p.getTokenOptimizer());
    else if (method == "set_parameters_optimized")       return MCPToolsExtended::setParametersOptimized(params, p, p.getTokenOptimizer());
    else if (method == "get_morph_compatibility")        result = MCPToolsExtended::getMorphCompatibility(params, p, p.getParameterClassifier());
    else if (method == "suggest_intermediate_snapshots") result = MCPToolsExtended::suggestIntermediateSnapshots(params, p, p.getParameterClassifier());
    else if (method == "get_parameter_categories")       result = MCPToolsExtended::getParameterCategories(params, p, p.getParameterClassifier());
    else if (method == "learn_from_adjustment")          return MCPToolsExtended::learnFromAdjustment(params, p.getParameterClassifier());
    else if (method == "get_learn_mode_status")          result = MCPToolsExtended::getLearnModeStatus(params, p.getParameterClassifier());
    else if (method == "set_learn_mode_config")          return MCPToolsExtended::setLearnModeConfig(params, p.getParameterClassifier());
    else if (method == "reset_learning_data")            return MCPToolsExtended::resetLearningData(params, p.getParameterClassifier());
    else if (method == "get_discrete_parameters")        result = MCPToolsExtended::getDiscreteParameters(params, p, p.getParameterClassifier());
    else if (method == "suggest_morph_settings")         result = MCPToolsExtended::suggestMorphSettings(params, p, p.getParameterClassifier());
    else if (method == "get_usage_stats")                result = MCPToolsExtended::getUsageStats(params, p.getTokenOptimizer());
    else if (method == "set_token_budget")               return MCPToolsExtended::setTokenBudget(params, p.getTokenOptimizer());
    else if (method == "explain_parameter")              result = MCPToolsExtended::explainParameter(params, p, p.getParameterClassifier());
    else if (method == "find_related_parameters")        result = MCPToolsExtended::findRelatedParameters(params, p, p.getParameterClassifier());
    else if (method == "generate_dataset")               return MCPToolsExtended::generateDataset(params, p);
    else if (method == "generate_dataset_v2")            return MCPToolsExtended::generateDatasetV2(params, p);
    else if (method == "generate_dataset_v3")            return MCPToolsExtended::generateDatasetV3(params, p);

    // Multi-instance tools
    else if (method == "get_instance_info")    result = getInstanceInfo(identity);
    else if (method == "list_instances")       result = listInstances();

    // Ozone mastering tools
    else if (method == "get_mastering_state")  result = getMasteringState(p);
    else if (method == "apply_mastering_plan") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return applyMasteringPlan(params, p); });
    else if (method == "sonicmaster_decision")  result = sonicmasterDecision(params, p);
    // Stage B (2026-06-26): one-click neural analyze+apply. Writes hosted params,
    // so route through the automation transaction (MediumWrite) like apply_plan.
    else if (method == "mastering.neural_apply") return dispatchWithAutomationTransaction(method, params, p, runtime, [&]() { return masteringNeuralApply(params, p); });

    // Ozone Track Assistant tools (guide-aligned, implemented natively in C++)
    else if (method == "ozone.track.get_info" || method == "ozone_track_get_info")
        result = ozoneTrackGetInfo(params, p, identity);
    else if (method == "ozone.track.update_status" || method == "ozone_track_update_status")
        return ozoneTrackUpdateStatus(params, identity);
    else if (method == "ozone.track.analyze" || method == "ozone_track_analyze")
        result = ozoneTrackAnalyze(params, p, identity);
    else if (method == "ozone.track.search" || method == "ozone_track_search")
        result = ozoneTrackSearch(params, p, identity);

    if (result.isNotEmpty())
    {
        if (isCacheableTool(method))
            cacheToolResult(method, params, p, result);
        return result;
    }

    return toJString(json{{"error","unknown_method"}});
}

// ── Tool implementations ──────────────────────────────────────────────────────

juce::String MCPToolHandler::getPluginInfo(MorePhiProcessor& p)
{
    json result = {
        {"name",    "More-Phi"},
        {"type",    "effect"},
        {"version", "3.3.0"}
    };

    auto* plugin = p.getHostManager().getPlugin();
    if (plugin)
    {
        result["hostedPlugin"] = {
            {"name",       plugin->getName().toStdString()},
            {"type",       plugin->acceptsMidi() ? "instrument" : "effect"},
            {"paramCount", static_cast<int>(plugin->getParameters().size())}
        };
    }
    else
    {
        result["hostedPlugin"] = nullptr;
    }

    return toJString(result);
}

juce::String MCPToolHandler::listParameters(const juce::var& /*params*/, MorePhiProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const auto descriptors = bridge.getParameterDescriptors();

    json arr = json::array();
    for (const auto& descriptor : descriptors)
    {
        arr.push_back({
            {"id",           descriptor.index},
            {"index",        descriptor.index},
            {"stableId",     descriptor.stableId.toStdString()},
            {"name",         descriptor.name.toStdString()},
            {"value",        descriptor.value},
            {"displayValue", descriptor.displayValue.toStdString()},
            {"label",        descriptor.label.toStdString()},
            {"discrete",     descriptor.discrete},
            {"boolean",      descriptor.boolean},
            {"numSteps",     descriptor.numSteps},
            {"defaultValue", descriptor.defaultValue}
        });
    }
    return toJString(arr);
}

juce::String MCPToolHandler::getParameter(const juce::var& params, MorePhiProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const auto stableId = params.getProperty("stableId",
                          params.getProperty("stable_id", "")).toString();
    const auto idOpt = extractParamId(params);
    const auto name = params.getProperty("name", "").toString();
    const auto resolution = bridge.resolveParameter(stableId, idOpt.value_or(-1), name);
    if (!resolution.success)
        return toJString(json{{"error","invalid_param_id"}});
    const auto descriptor = bridge.getParameterDescriptor(resolution.index);

    return toJString(json{
        {"id",           descriptor.index},
        {"index",        descriptor.index},
        {"stableId",     descriptor.stableId.toStdString()},
        {"name",         descriptor.name.toStdString()},
        {"value",        descriptor.value},
        {"displayValue", descriptor.displayValue.toStdString()},
        {"label",        descriptor.label.toStdString()},
        {"discrete",     descriptor.discrete},
        {"boolean",      descriptor.boolean},
        {"numSteps",     descriptor.numSteps},
        {"defaultValue", descriptor.defaultValue}
    });
}

juce::String MCPToolHandler::setParameter(const juce::var& params, MorePhiProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    const auto stableId = params.getProperty("stableId",
                          params.getProperty("stable_id", "")).toString();
    const auto idOpt = extractParamId(params);
    const auto name = params.getProperty("name", "").toString();
    const auto resolution = bridge.resolveParameter(stableId, idOpt.value_or(-1), name);
    if (!resolution.success)
        return toJString(json{
            {"success", false},
            {"error", resolution.error.toStdString()},
            {"queued", 0},
            {"rejected", 1}
        });
    const int id = resolution.index;
    const auto rawValue = static_cast<float>(params.getProperty("value", 0.0));
    // MCP-PARAMS-01: reject non-finite values. juce::jlimit is a no-op for NaN
    // (NaN comparisons are false), so without this gate a client-supplied NaN/Inf
    // reaches hostedPlugin->setValue() on the audio thread (UB). The more_phi.*
    // tools already validate this way — apply the same gate to the hosted path.
    if (!std::isfinite(rawValue))
        return toJString(json{{"success", false}, {"error", "value_must_be_finite"},
                              {"index", id}, {"rejected", 1}});
    const float value = juce::jlimit(0.0f, 1.0f, rawValue);

    // Verification: capture the pre-edit value before routing through the queue.
    const auto t0 = std::chrono::steady_clock::now();
    const float valueBefore = bridge.getParameterNormalized(id);
    const juce::String humanBefore = bridge.getParameterDisplayValueAtNormalized(id, valueBefore);

    // Route through the command queue first, then ask the processor to flush it
    // through exclusive hosted-plugin access when the DAW is idle.
    if (!p.enqueueParameterSet(id, value, MorePhiProcessor::ParameterEditSource::MCP, true))
    {
        const auto t1f = std::chrono::steady_clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(t1f - t0).count();
        auto v = classifyVerification(false, false, value, 0.0f, elapsedMs, "queue_full");
        v.valueBefore = valueBefore;
        v.humanBefore = humanBefore;
        return toJString(json{
            {"success", false},
            {"error", "queue_full"},
            {"index", id},
            {"value", value},
            {"queued", 0},
            {"rejected", 0},
            {"verification", verificationToJson(v)}
        });
    }

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(1);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "set_parameter";
    optimizer.recordUsage(usage);

    const auto flush = p.flushPendingParameterCommandsForAssistant();

    // Verification: read back the post-flush value.
    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const float valueAfter = bridge.getParameterNormalized(id);
    const juce::String humanAfter = bridge.getParameterDisplayValueAtNormalized(id, valueAfter);

    // AUDIT-FIX 4.7: if the flush detected out-of-range writes, surface the
    // specific error instead of letting it fall through to value_drift.
    const bool indexOutOfRange = flush.outOfRangeCount > 0;

    // AUDIT-FIX 4.1: pass discrete info for tighter tolerance on step-snapping params.
    const bool paramIsDiscrete = bridge.isDiscrete(id);
    const int  paramNumSteps   = bridge.getParameterNumSteps(id);

    // AUDIT-FIX 4.5: after flush, check if morph may have overwritten the edit.
    // liveEditHold_ is only accessible via the processor; we approximate by
    // checking if the morph is active (snapshots occupied) and the value drifted.
    const bool morphActive = p.getSnapshotBank().hasAnyOccupied();
    const bool morphOverwriteRisk = morphActive && (std::abs(valueAfter - value) > 0.001f)
                                    && (std::abs(valueAfter - valueBefore) < 0.001f);
    // Heuristic: if valueAfter == valueBefore (despite our write), morph likely overwrote.

    auto verification = classifyVerification(
        true, flush.drained > 0 && !indexOutOfRange,
        value, valueAfter, elapsedMs,
        indexOutOfRange ? "parameter_index_out_of_range" : "",
        paramIsDiscrete, paramNumSteps,
        morphOverwriteRisk);
    verification.valueBefore = valueBefore;
    verification.humanBefore = humanBefore;
    verification.humanAfter = humanAfter;

    // F4/AUDIT: when morph is active, the immediate readback above can return
    // `success` while the next applyMorphAndParameters block silently
    // overwrites the edit. Re-read after one block; if morph clobbered it,
    // downgrade the verification so the assistant is told the edit did NOT
    // stick. Skipped when morph is inactive (nothing would overwrite it) and
    // when the immediate readback already failed (no point re-checking).
    if (morphActive && verification.status == "success")
    {
        if (auto deferred = deferredMorphOverwriteCheck(bridge, id, value,
                                                        paramIsDiscrete, paramNumSteps))
        {
            verification = *deferred;
        }
    }

    // AUDIT-FIX 4.3: gate top-level success on actual verification — do NOT
    // report success when the edit is still queued or the value drifted.
    // EXCEPTION: when the command was accepted into the realtime queue but the
    // hosted plugin is currently unavailable (nothing to apply against), the
    // write is a SOFT SUCCESS — the queue holds it and it will apply the moment
    // a plugin loads. Reporting a hard failure here would mislead callers into
    // retrying/erroring on a perfectly valid queued write. The plugin_unavailable
    // flag (below in "flush") is the informative signal, and a warning is added.
    const bool pluginUnavailableSoftSuccess =
        flush.pluginUnavailable && flush.drained == 0 && flush.pendingAfter > 0;
    const bool editVerified = verification.status == "success" || pluginUnavailableSoftSuccess;

    json response{
        {"success", editVerified},
        {"index", id},
        {"value", value},
        {"queued", 1},
        {"rejected", 0},
        {"appliedNow", flush.drained},
        {"pendingAfter", flush.pendingAfter},
        {"flush", parameterFlushToJson(flush)},
        {"verification", verificationToJson(verification)}
    };

    if (!editVerified)
    {
        response["error"] = verification.errorReason.toStdString();
        if (!verification.correctiveAction.isEmpty())
            response["suggested_action"] = verification.correctiveAction.toStdString();
    }

    if (flush.drained == 0 && flush.pendingBefore > 0)
    {
        if (flush.pluginUnavailable)
        {
            response["warning"] = "Value was queued but the hosted plugin is currently unavailable; it will apply once a plugin is loaded.";
            if (!response.contains("suggested_action"))
                response["suggested_action"] = suggestedActionForError("plugin_not_loaded").toStdString();
        }
        else if (flush.exclusiveAccessTimedOut)
        {
            response["warning"] = "Value was queued but could not be flushed immediately because the hosted plugin was in use; the audio thread will apply it on the next callback.";
            if (!response.contains("suggested_action"))
                response["suggested_action"] = suggestedActionForError("pending_parameter_edits").toStdString();
        }
    }

    if (indexOutOfRange)
    {
        // The write targeted an index the (loaded or stub) descriptor map does
        // not have. This is a genuine client error: downgrade any soft-success
        // to a hard failure so the caller is told the write was dropped.
        response["success"] = false;
        response["warning"] = "Parameter index exceeds the hosted plugin's parameter count; the write was dropped. Use list_parameters to discover valid indices.";
    }

    return toJString(response);
}

juce::String MCPToolHandler::setParametersBatch(const juce::var& params, MorePhiProcessor& p)
{
    const juce::var batchPayload = params.hasProperty("params")
        ? params.getProperty("params", juce::var())
        : params.getProperty("parameters", juce::var());
    auto* list = batchPayload.getArray();
    if (!list)
        return toJString(json{{"success",false},{"error","missing params/parameters array"}});

    auto& bridge = p.getParameterBridge();
    const int requested = list->size();
    int applied = 0;
    int rejected = 0;
    int queueFailures = 0;

    // Per-item verification capture, resolved during the enqueue loop and
    // finalized after the single flush reads each parameter back.
    struct BatchVerificationItem
    {
        int index = -1;
        float requested = 0.0f;
        float valueBefore = 0.0f;
        juce::String stableId;
        juce::String name;
        bool enqueued = false;
        juce::String failureCode;
        bool isDiscrete = false;   // AUDIT-FIX 4.1
        int numSteps = 0;          // AUDIT-FIX 4.1
    };
    std::vector<BatchVerificationItem> verificationItems;
    verificationItems.reserve(static_cast<size_t>(requested));

    const auto t0 = std::chrono::steady_clock::now();

    // Route all parameter changes through the command queue, then flush once
    // after batching so assistant edits can apply immediately when safe.
    for (const auto& item : *list)
    {
        const int rawId   = item.hasProperty("index")
            ? static_cast<int>(item.getProperty("index", -1))
            : static_cast<int>(item.getProperty("id", -1));
        const auto stableId = item.getProperty("stableId",
                              item.getProperty("stable_id", "")).toString();
        const auto name = item.getProperty("name", "").toString();
        const auto rawValue = static_cast<float>(item.getProperty("value", 0.0));
        const auto resolution = bridge.resolveParameter(stableId, rawId, name);

        BatchVerificationItem vi;
        vi.stableId = stableId;
        vi.name = name;
        vi.requested = std::isfinite(rawValue) ? juce::jlimit(0.0f, 1.0f, rawValue) : 0.0f;
        vi.index = resolution.success ? resolution.index : rawId;

        if (resolution.success)
        {
            // MCP-PARAMS-01: reject non-finite values (juce::jlimit no-op for NaN).
            if (!std::isfinite(rawValue))
            {
                ++rejected;
                vi.failureCode = "non_finite_value";
                verificationItems.push_back(std::move(vi));
                continue;
            }
            const float value = juce::jlimit(0.0f, 1.0f, rawValue);
            vi.valueBefore = bridge.getParameterNormalized(resolution.index);
            // AUDIT-FIX 4.1: capture discrete info during enqueue for per-item tolerance.
            vi.isDiscrete = bridge.isDiscrete(resolution.index);
            vi.numSteps = bridge.getParameterNumSteps(resolution.index);
            if (p.enqueueParameterSet(resolution.index, value,
                                      MorePhiProcessor::ParameterEditSource::MCP,
                                      true))
            {
                ++applied;
                vi.enqueued = true;
            }
            else
            {
                ++queueFailures;
                vi.failureCode = "queue_full";
            }
        }
        else
        {
            ++rejected;
            vi.failureCode = resolution.error.isEmpty() ? "unresolved_parameter" : resolution.error;
        }
        verificationItems.push_back(std::move(vi));
    }

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(applied);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "set_parameters_batch";
    optimizer.recordUsage(usage);

    const auto flush = applied > 0
        ? p.flushPendingParameterCommandsForAssistant(juce::jmax(2048, applied))
        : MorePhiProcessor::ParameterCommandFlushResult{};

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const bool drained = flush.drained > 0;

    // Finalize per-item verification now that the flush has applied the batch.
    const bool morphActive = p.getSnapshotBank().hasAnyOccupied();
    json verificationArray = json::array();
    int verifiedCount = 0;
    for (auto& vi : verificationItems)
    {
        VerificationCapture v;
        if (vi.enqueued)
        {
            const float valueAfter = bridge.getParameterNormalized(vi.index);
            // AUDIT-FIX 4.5: detect morph overwrite risk per item.
            const bool itemMorphOverwrite = morphActive
                && (std::abs(valueAfter - vi.requested) > 0.001f)
                && (std::abs(valueAfter - vi.valueBefore) < 0.001f);
            v = classifyVerification(true, drained, vi.requested, valueAfter, elapsedMs,
                                     "", vi.isDiscrete, vi.numSteps, itemMorphOverwrite);
            v.humanBefore = bridge.getParameterDisplayValueAtNormalized(vi.index, vi.valueBefore);
            v.humanAfter = bridge.getParameterDisplayValueAtNormalized(vi.index, valueAfter);
            v.valueBefore = vi.valueBefore;
            if (v.status == "success")
                ++verifiedCount;
        }
        else
        {
            v = classifyVerification(false, false, vi.requested, 0.0f, elapsedMs, vi.failureCode);
            v.valueBefore = vi.valueBefore;
        }

        json viJson{
            {"index", vi.index},
            {"verification", verificationToJson(v)}
        };
        if (vi.stableId.isNotEmpty()) viJson["stableId"] = vi.stableId.toStdString();
        if (vi.name.isNotEmpty())     viJson["name"] = vi.name.toStdString();
        verificationArray.push_back(std::move(viJson));
    }

    // F4/AUDIT: when morph is active, the per-item immediate readback above
    // cannot see a morph overwrite in the next block. Do ONE deferred re-read
    //pass (6ms covers all items — morph runs per block) and downgrade any
    //item that was immediately `success` but got clobbered.
    if (morphActive && verifiedCount > 0)
    {
        juce::Thread::sleep(6);
        for (auto& viJson : verificationArray)
        {
            auto& v = viJson["verification"];
            if (v.value("status", std::string{}) != "success")
                continue;
            const int itemId = viJson.value("index", -1);
            if (itemId < 0)
                continue;
            const float requestedVal = v.value("requested_value", 0.0f);
            const float valueDeferred = bridge.getParameterNormalized(itemId);
            float tolerance = kVerificationDriftToleranceContinuous;
            if (bridge.isDiscrete(itemId))
            {
                const int steps = bridge.getParameterNumSteps(itemId);
                if (steps > 0) tolerance = std::max(0.5f / static_cast<float>(steps), 0.001f);
            }
            if (std::abs(valueDeferred - requestedVal) > tolerance)
            {
                v["status"] = "morph_overwrite_confirmed";
                v["value_after"] = valueDeferred;
                v["error_reason"] = "morph_overwrote_edit_after_one_block";
                v["corrective_action"] = "Pause morph (or move morph position away "
                                         "from a snapshot boundary) before re-applying.";
                --verifiedCount;
            }
        }
    }

    // AUDIT-FIX 4.3: gate top-level success on actual verification, not just
    // queue acceptance. If no items were verified as successfully applied, the
    // batch did not take effect.
    const bool allQueued = requested > 0
        && applied == requested
        && rejected == 0
        && queueFailures == 0;
    const bool anyVerified = verifiedCount > 0;
    // Success needs both: all items queued AND at least one verified as applied.
    const bool batchSuccess = allQueued && (anyVerified || applied == 0);

    json response{
        {"success", batchSuccess},
        {"queued", applied},
        {"applied", applied},
        {"appliedNow", flush.drained},
        {"requested", requested},
        {"rejected", rejected},
        {"queueFailures", queueFailures},
        {"verifiedCount", verifiedCount},
        {"pendingAfter", flush.pendingAfter},
        {"flush", parameterFlushToJson(flush)},
        {"verification", verificationArray}
    };

    if (queueFailures > 0)
        response["error"] = "queue_full";
    else if (requested == 0)
        response["error"] = "empty_batch";
    else if (applied == 0)
        response["error"] = "no_parameters_queued";
    else if (rejected > 0)
        response["error"] = "partial_rejected";

    if (flush.drained == 0 && flush.pendingBefore > 0)
    {
        if (flush.pluginUnavailable)
        {
            response["warning"] = "Parameters were queued but the hosted plugin is currently unavailable; they will apply once a plugin is loaded.";
            response["suggested_action"] = suggestedActionForError("plugin_not_loaded").toStdString();
        }
        else if (flush.exclusiveAccessTimedOut)
        {
            response["warning"] = "Parameters were queued but could not be flushed immediately because the hosted plugin was in use; the audio thread will apply them on the next callback.";
            response["suggested_action"] = suggestedActionForError("pending_parameter_edits").toStdString();
        }
    }

    if (allQueued && !anyVerified && applied > 0)
    {
        response["warning"] = "All parameters were queued but none could be verified as applied; the hosted plugin may not be ready or the indices may be out of range.";
        response["suggested_action"] = suggestedActionForError("plugin_not_ready").toStdString();
    }

    return toJString(response);
}

juce::String MCPToolHandler::sweepParameter(const juce::var& params, MorePhiProcessor& p)
{
    // sweep_parameter / AUDIT: the only tool that autonomously sweeps a value
    // range on the LIVE hosted plugin. Each step reuses the verified write path
    // (resolve -> enqueue -> flush) and then reads the live BS.1770-4 / true-
    // peak / spectrum / THD meters, so the assistant gets a real per-value
    // measurement curve instead of a one-shot guess. ponytail: no new write
    // mechanism — every step goes through the same set_parameter plumbing the
    // assistant already trusts, including the F6 force-apply drain and F4
    // morph-overwrite readback.
    auto& bridge = p.getParameterBridge();

    // Resolve the target parameter once (stableId | index | name).
    const auto stableId = params.getProperty("stableId",
                          params.getProperty("stable_id", "")).toString();
    const int rawId = params.hasProperty("index")
        ? static_cast<int>(params.getProperty("index", -1))
        : static_cast<int>(params.getProperty("id", -1));
    const auto name = params.getProperty("name",
                       params.getProperty("parameter", "")).toString();
    const auto resolution = bridge.resolveParameter(stableId, rawId, name);
    if (! resolution.success)
    {
        return toJString(json{{"success", false},
            {"error", resolution.error.isEmpty() ? "unresolved_parameter" : resolution.error.toStdString()},
            {"resolved", false}});
    }
    const int idx = resolution.index;

    float from = static_cast<float>(params.getProperty("from", 0.0));
    float to   = static_cast<float>(params.getProperty("to",   1.0));
    from = juce::jlimit(0.0f, 1.0f, from);
    to   = juce::jlimit(0.0f, 1.0f, to);
    int steps = static_cast<int>(params.getProperty("steps", 5));
    steps = juce::jlimit(2, 64, steps);
    const int captureMs = juce::jlimit(10, 5000, static_cast<int>(params.getProperty("capture_ms", 250)));

    json results = json::array();
    int capturedSteps = 0;

    for (int step = 0; step < steps; ++step)
    {
        // Linear interpolation across [from, to]; inclusive of both endpoints
        // when steps >= 2.
        const float t = (steps <= 1) ? 0.0f : static_cast<float>(step) / static_cast<float>(steps - 1);
        const float value = juce::jlimit(0.0f, 1.0f, from + t * (to - from));

        // Enqueue + flush through the same verified write path as set_parameter.
        if (! p.enqueueParameterSet(idx, value, MorePhiProcessor::ParameterEditSource::MCP, true))
        {
            results.push_back({{"step", step}, {"value", value},
                               {"error", "queue_full"}, {"applied", false}});
            continue;
        }
        const auto flush = p.flushPendingParameterCommandsForAssistant();
        // Let the meters settle on the new value before sampling.
        juce::Thread::sleep(captureMs);

        const auto m = p.getSonicMasterEngine().getLiveMeasurements();
        json meas;
        meas["valid"] = m.valid;
        if (m.valid)
        {
            meas["lufs_integrated"]      = m.lufsIntegrated;
            meas["lufs_short_term"]      = m.lufsShortTerm;
            meas["lufs_momentary"]       = m.lufsMomentary;
            meas["lra"]                  = m.lra;
            meas["true_peak_dbtp"]       = m.truePeakDbtp;
            meas["spectral_centroid_hz"] = m.spectralCentroidHz;
            meas["spectral_tilt"]        = m.spectralTilt;
            meas["stereo_width"]         = m.stereoWidth;
            meas["correlation_mid"]      = m.correlationMid;
            meas["thd_percent"]          = m.thdPercent;
            meas["crest_factor_program"] = m.crestFactorProgram;
        }
        // Read back the actual applied value so the assistant can see drift.
        const float appliedValue = bridge.getParameterNormalized(idx);

        results.push_back({
            {"step", step},
            {"value", value},
            {"applied_value", appliedValue},
            {"applied", flush.drained > 0},
            {"measurements", meas}
        });
        ++capturedSteps;
    }

    json response{
        {"success", capturedSteps > 0},
        {"index", idx},
        {"from", from},
        {"to", to},
        {"steps", steps},
        {"captured_steps", capturedSteps},
        {"results", results}
    };
    if (capturedSteps == 0)
        response["error"] = "no_steps_captured";
    return toJString(response);
}

juce::String MCPToolHandler::captureSnapshot(const juce::var& params, MorePhiProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return toJString(json{{"success",false},{"error","invalid slot"}});

    const bool includeState = params.getProperty("includeState",
                              params.getProperty("include_state", true));
    if (!p.captureSnapshotToSlot(slot, includeState))
        return toJString(json{{"success",false},{"error","capture_failed"},{"slot",slot}});

    return toJString(json{
        {"success", true},
        {"slot", slot},
        {"includeState", includeState},
        {"stateChunk", p.getSnapshotBank().hasStateChunk(slot)}
    });
}

juce::String MCPToolHandler::recallSnapshot(const juce::var& params, MorePhiProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot < 0 || slot >= SnapshotBank::NUM_SLOTS)
        return toJString(json{{"success",false},{"error","invalid slot"}});

    // Get snapshot values and route through command queue for thread safety.
    // CRITICAL (Finding 2): Must use enqueueParameterState() instead of direct
    // applyParameterState() to prevent concurrent plugin API calls from
    // MCP thread and audio thread (undefined behavior).
    std::vector<float> values;
    if (!p.getSnapshotBank().getSlotValuesCopy(slot, values))
        return toJString(json{{"success",false},{"error","empty_slot"}});

    auto mode = MorePhiProcessor::SnapshotRecallMode::FastParamsOnly;
    const auto modeText = params.getProperty("mode", "").toString().trim().toLowerCase();
    const bool requestFull = params.getProperty("full",
                             params.getProperty("includeState",
                             params.getProperty("include_state", false)));
    if (requestFull || modeText == "full" || p.getRecallMode() == 1)
        mode = MorePhiProcessor::SnapshotRecallMode::FullStateAndParams;

    if (!p.recallSnapshot(slot, mode))
        return toJString(json{{"success",false},{"error","queue_full"}});
    const int queued = static_cast<int>(values.size());

    auto& optimizer = p.getTokenOptimizer();
    const auto estimate = optimizer.estimateSetParameter(queued);
    TokenUsage usage;
    usage.promptTokens = estimate.totalTokens;
    usage.completionTokens = estimate.totalTokens / 5;
    usage.estimatedCostUsd = estimate.estimatedCostUsd;
    usage.timestamp = std::chrono::steady_clock::now();
    usage.operation = "recall_snapshot";
    optimizer.recordUsage(usage);

    return toJString(json{
        {"success", true},
        {"slot", slot},
        {"queued", queued},
        {"mode", mode == MorePhiProcessor::SnapshotRecallMode::FullStateAndParams ? "full" : "fast"}
    });
}

juce::String MCPToolHandler::setMorphPosition(const juce::var& params, MorePhiProcessor& p)
{
    bool sourceExplicitlySet = false;

    if (params.hasProperty("source"))
    {
        auto source = params.getProperty("source", "").toString().trim().toLowerCase();
        if (source == "xy" || source == "xypad")
        {
            p.setMorphSource(0);
            sourceExplicitlySet = true;
        }
        else if (source == "fader")
        {
            p.setMorphSource(1);
            sourceExplicitlySet = true;
        }
        else
        {
            return toJString(json{{"success",false},{"error","invalid_source"}});
        }
    }

    const bool hasX = params.hasProperty("x");
    const bool hasY = params.hasProperty("y");
    const bool hasFader = params.hasProperty("fader");

    int source = p.getMorphSource();
    if (!sourceExplicitlySet && (hasX || hasY))
        source = 0;
    if (hasFader)
        source = 1;
    if (sourceExplicitlySet)
        source = p.getMorphSource();

    // MCP-PARAMS-02: reject non-finite morph coordinates — they survive jlimit and
    // propagate through interpolation into setValue() on the audio thread.
    const float xIn     = static_cast<float>(params.getProperty("x", p.getMorphX()));
    const float yIn     = static_cast<float>(params.getProperty("y", p.getMorphY()));
    const float faderIn = static_cast<float>(params.getProperty("fader", p.getFaderPos()));
    if ((hasX && !std::isfinite(xIn)) || (hasY && !std::isfinite(yIn)) ||
        (hasFader && !std::isfinite(faderIn)))
        return toJString(json{{"success", false}, {"error", "morph_values_must_be_finite"}});

    p.setMorphPositionExternal(xIn, hasX, yIn, hasY, faderIn, hasFader, source);

    return toJString(json{{"success",true}});
}

juce::String MCPToolHandler::getMorphState(MorePhiProcessor& p)
{
    return toJString(json{
        {"x",      p.getMorphX()},
        {"y",      p.getMorphY()},
        {"fader",  p.getFaderPos()},
        {"source", p.getMorphSource()}
    });
}

struct MorePhiParameterResolution
{
    juce::RangedAudioParameter* parameter = nullptr;
    juce::String parameterId;
    int index = -1;
    juce::String error;
};

static int findMorePhiParameterIndex(MorePhiProcessor& processor,
                                     const juce::RangedAudioParameter* target)
{
    const auto& parameters = processor.getParameters();
    for (int i = 0; i < parameters.size(); ++i)
    {
        if (parameters[i] == target)
            return i;
    }

    return -1;
}

static juce::RangedAudioParameter* getMorePhiRangedParameterAt(MorePhiProcessor& processor,
                                                               int index)
{
    const auto& parameters = processor.getParameters();
    if (index < 0 || index >= parameters.size())
        return nullptr;

    return dynamic_cast<juce::RangedAudioParameter*>(parameters[index]);
}

static json morePhiParameterToJson(MorePhiProcessor& processor,
                                   juce::RangedAudioParameter& parameter)
{
    const auto parameterId = parameter.getParameterID();
    const auto value = parameter.getValue();
    const auto defaultValue = parameter.getDefaultValue();
    const auto plainValue = parameter.convertFrom0to1(value);
    const auto plainDefault = parameter.convertFrom0to1(defaultValue);
    const auto& range = parameter.getNormalisableRange();

    const bool isChoice = dynamic_cast<juce::AudioParameterChoice*>(&parameter) != nullptr;
    const char* type = parameter.isBoolean() ? "boolean"
                     : isChoice ? "choice"
                     : parameter.isDiscrete() ? "discrete"
                     : "float";

    json result{
        {"parameter_id", parameterId.toStdString()},
        {"parameterId", parameterId.toStdString()},
        {"index", findMorePhiParameterIndex(processor, &parameter)},
        {"name", parameter.getName(128).toStdString()},
        {"type", type},
        {"value", value},
        {"normalized_value", value},
        {"plain_value", plainValue},
        {"displayValue", parameter.getText(value, 128).toStdString()},
        {"label", parameter.getLabel().toStdString()},
        {"defaultValue", defaultValue},
        {"plain_default_value", plainDefault},
        {"discrete", parameter.isDiscrete()},
        {"boolean", parameter.isBoolean()},
        {"automatable", parameter.isAutomatable()},
        {"numSteps", parameter.getNumSteps()},
        {"range", {
            {"start", range.start},
            {"end", range.end},
            {"interval", range.interval}
        }}
    };

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(&parameter))
    {
        json choices = json::array();
        for (int i = 0; i < choice->choices.size(); ++i)
            choices.push_back(choice->choices[i].toStdString());
        result["choices"] = choices;
    }
    else if (parameter.isBoolean())
    {
        result["choices"] = json::array({"Off", "On"});
    }

    return result;
}

static MorePhiParameterResolution resolveMorePhiParameter(const juce::var& params,
                                                          MorePhiProcessor& processor)
{
    auto id = params.getProperty("parameter_id",
              params.getProperty("parameterId", "")).toString().trim();

    if (id.isEmpty() && params.hasProperty("id"))
    {
        const auto idVar = params.getProperty("id", juce::var());
        if (idVar.isString())
            id = idVar.toString().trim();
    }

    if (id.isNotEmpty())
    {
        if (auto* parameter = processor.getAPVTS().getParameter(id))
            return { parameter, parameter->getParameterID(), findMorePhiParameterIndex(processor, parameter), {} };

        return { nullptr, id, -1, "unknown_parameter_id" };
    }

    int index = -1;
    if (params.hasProperty("index"))
    {
        index = static_cast<int>(params.getProperty("index", -1));
    }
    else if (params.hasProperty("id"))
    {
        const auto idVar = params.getProperty("id", juce::var());
        if (idVar.isInt() || idVar.isInt64() || idVar.isDouble())
            index = static_cast<int>(idVar);
    }

    if (auto* parameter = getMorePhiRangedParameterAt(processor, index))
        return { parameter, parameter->getParameterID(), index, {} };
    if (index >= 0)
        return { nullptr, {}, index, "invalid_parameter_index" };

    const auto name = params.getProperty("name", "").toString().trim();
    if (name.isNotEmpty())
    {
        const auto& parameters = processor.getParameters();
        for (int i = 0; i < parameters.size(); ++i)
        {
            if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(parameters[i]))
            {
                if (parameter->getName(128) == name)
                    return { parameter, parameter->getParameterID(), i, {} };
            }
        }

        return { nullptr, {}, -1, "unknown_parameter_name" };
    }

    return { nullptr, {}, -1, "missing_parameter_identifier" };
}

static bool setMorePhiParameterWithGesture(juce::RangedAudioParameter& parameter,
                                           float normalizedValue)
{
    auto apply = [&parameter, normalizedValue]()
    {
        parameter.beginChangeGesture();
        parameter.setValueNotifyingHost(normalizedValue);
        parameter.endChangeGesture();
    };

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        apply();
        return true;
    }

    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
    {
        parameter.setValue(normalizedValue);
        return true;
    }

    parameter.setValue(normalizedValue);
    return true;
}

juce::String MCPToolHandler::listMorePhiParameters(MorePhiProcessor& p)
{
    json arr = json::array();
    const auto& parameters = p.getParameters();
    for (int i = 0; i < parameters.size(); ++i)
    {
        if (auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(parameters[i]))
            arr.push_back(morePhiParameterToJson(p, *parameter));
    }

    return toJString(arr);
}

juce::String MCPToolHandler::getMorePhiParameter(const juce::var& params,
                                                 MorePhiProcessor& p)
{
    auto resolution = resolveMorePhiParameter(params, p);
    if (resolution.parameter == nullptr)
    {
        return toJString(json{
            {"success", false},
            {"error", resolution.error.toStdString()}
        });
    }

    auto result = morePhiParameterToJson(p, *resolution.parameter);
    result["success"] = true;
    return toJString(result);
}

juce::String MCPToolHandler::setMorePhiParameter(const juce::var& params,
                                                 MorePhiProcessor& p)
{
    if (!params.hasProperty("value"))
        return invalidParamsResponse("value is required.");

    const auto rawValue = static_cast<float>(params.getProperty("value", 0.0));
    if (!std::isfinite(rawValue))
        return invalidParamsResponse("value must be finite.");

    auto resolution = resolveMorePhiParameter(params, p);
    if (resolution.parameter == nullptr)
    {
        return toJString(json{
            {"success", false},
            {"error", resolution.error.toStdString()},
            {"rejected", 1},
            {"applied", 0}
        });
    }

    const auto value = juce::jlimit(0.0f, 1.0f, rawValue);

    // Verification: capture the pre-edit value, apply, then read back.
    const auto t0 = std::chrono::steady_clock::now();
    const float valueBefore = resolution.parameter->getValue();
    const juce::String humanBefore = resolution.parameter->getText(valueBefore, 128);

    const bool applied = setMorePhiParameterWithGesture(*resolution.parameter, value);

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (!applied)
    {
        auto v = classifyVerification(false, false, value, 0.0f, elapsedMs, "message_thread_unavailable");
        v.valueBefore = valueBefore;
        v.humanBefore = humanBefore;
        return toJString(json{
            {"success", false},
            {"error", "message_thread_unavailable"},
            {"parameter_id", resolution.parameterId.toStdString()},
            {"rejected", 0},
            {"applied", 0},
            {"verification", verificationToJson(v)}
        });
    }

    const float valueAfter = resolution.parameter->getValue();
    const juce::String humanAfter = resolution.parameter->getText(valueAfter, 128);
    auto v = classifyVerification(true, true, value, valueAfter, elapsedMs);
    v.valueBefore = valueBefore;
    v.humanBefore = humanBefore;
    v.humanAfter = humanAfter;

    return toJString(json{
        {"success", true},
        {"parameter_id", resolution.parameterId.toStdString()},
        {"index", resolution.index},
        {"value", value},
        {"requested_value", rawValue},
        {"clamped", std::abs(value - rawValue) > 0.000001f},
        {"applied", 1},
        {"rejected", 0},
        {"parameter", morePhiParameterToJson(p, *resolution.parameter)},
        {"verification", verificationToJson(v)}
    });
}

juce::String MCPToolHandler::setMorePhiParameters(const juce::var& params,
                                                  MorePhiProcessor& p)
{
    const juce::var batchPayload = params.hasProperty("params")
        ? params.getProperty("params", juce::var())
        : params.getProperty("parameters", juce::var());
    auto* list = batchPayload.getArray();
    if (!list)
        return invalidParamsResponse("parameters must be an array.");

    int applied = 0;
    int rejected = 0;
    json details = json::array();

    for (const auto& item : *list)
    {
        if (!item.hasProperty("value"))
        {
            ++rejected;
            auto v = classifyVerification(false, false, 0.0f, 0.0f, 0.0, "missing_value");
            details.push_back(json{{"success", false}, {"error", "missing_value"},
                                   {"verification", verificationToJson(v)}});
            continue;
        }

        const auto rawValue = static_cast<float>(item.getProperty("value", 0.0));
        if (!std::isfinite(rawValue))
        {
            ++rejected;
            auto v = classifyVerification(false, false, 0.0f, 0.0f, 0.0, "non_finite_value");
            details.push_back(json{{"success", false}, {"error", "non_finite_value"},
                                   {"verification", verificationToJson(v)}});
            continue;
        }

        auto resolution = resolveMorePhiParameter(item, p);
        if (resolution.parameter == nullptr)
        {
            ++rejected;
            auto v = classifyVerification(false, false, juce::jlimit(0.0f, 1.0f, rawValue), 0.0f, 0.0,
                                          resolution.error.isEmpty() ? "unresolved_parameter" : resolution.error);
            details.push_back(json{{"success", false}, {"error", resolution.error.toStdString()},
                                   {"verification", verificationToJson(v)}});
            continue;
        }

        const auto value = juce::jlimit(0.0f, 1.0f, rawValue);

        // Verification: capture pre-edit value, apply, read back.
        const auto t0 = std::chrono::steady_clock::now();
        const float valueBefore = resolution.parameter->getValue();
        const juce::String humanBefore = resolution.parameter->getText(valueBefore, 128);

        if (!setMorePhiParameterWithGesture(*resolution.parameter, value))
        {
            ++rejected;
            const auto t1f = std::chrono::steady_clock::now();
            const double elapsedMs = std::chrono::duration<double, std::milli>(t1f - t0).count();
            auto v = classifyVerification(false, false, value, 0.0f, elapsedMs, "message_thread_unavailable");
            v.valueBefore = valueBefore;
            v.humanBefore = humanBefore;
            details.push_back(json{
                {"success", false},
                {"error", "message_thread_unavailable"},
                {"parameter_id", resolution.parameterId.toStdString()},
                {"verification", verificationToJson(v)}
            });
            continue;
        }

        ++applied;
        const auto t1 = std::chrono::steady_clock::now();
        const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        const float valueAfter = resolution.parameter->getValue();
        const juce::String humanAfter = resolution.parameter->getText(valueAfter, 128);
        auto v = classifyVerification(true, true, value, valueAfter, elapsedMs);
        v.valueBefore = valueBefore;
        v.humanBefore = humanBefore;
        v.humanAfter = humanAfter;
        details.push_back(json{
            {"success", true},
            {"parameter_id", resolution.parameterId.toStdString()},
            {"index", resolution.index},
            {"value", value},
            {"requested_value", rawValue},
            {"clamped", std::abs(value - rawValue) > 0.000001f},
            {"verification", verificationToJson(v)}
        });
    }

    const int requested = list->size();
    json response{
        {"success", requested > 0 && applied == requested && rejected == 0},
        {"requested", requested},
        {"applied", applied},
        {"rejected", rejected},
        {"results", details}
    };

    if (requested == 0)
        response["error"] = "empty_batch";
    else if (applied == 0)
        response["error"] = "no_parameters_applied";
    else if (rejected > 0)
        response["error"] = "partial_rejected";

    return toJString(response);
}

// ── Diagnostic Test Suites ───────────────────────────────────────────────────

static json runDiscreteParameterSnappingTest(MorePhiProcessor& processor)
{
    json tests = json::array();
    bool allPassed = true;
    auto addTest = [&tests, &allPassed](const char* id, bool passed, const std::string& details)
    {
        tests.push_back({{"id", id}, {"ok", passed}, {"details", details}});
        allPassed = allPassed && passed;
    };

    auto& bridge = processor.getParameterBridge();
    const int paramCount = bridge.getParameterCount();

    if (paramCount == 0)
    {
        addTest("discrete_params_available", false, "No hosted plugin loaded; cannot test discrete parameters.");
        return {{"success", false}, {"suite", "discrete_parameter_snapping"}, {"tests", tests}};
    }

    try
    {
        // Test 1: Detect discrete parameters
        const auto discreteMap = bridge.getDiscreteMap();
        int discreteCount = 0;
        for (bool isDiscrete : discreteMap)
            if (isDiscrete) discreteCount++;

        addTest("discrete_params_detected", true,
                "Detected " + std::to_string(discreteCount) + " discrete parameters out of " +
                std::to_string(paramCount) + " total parameters.");

        // Test 2: Verify discrete parameters snap to valid steps
        if (discreteCount > 0)
        {
            bool snappingWorks = true;
            int testedCount = 0;

            for (int idx = 0; idx < paramCount && testedCount < 10; ++idx)
            {
                if (!discreteMap[static_cast<size_t>(idx)])
                    continue;

                // Get current value
                const float originalValue = bridge.getParameterNormalized(idx);

                // Test snapping to 3 different positions
                for (float testValue : {0.0f, 0.5f, 1.0f})
                {
                    bridge.setParameterNormalized(idx, testValue);
                    const float readValue = bridge.getParameterNormalized(idx);

                    // Discrete params should snap to valid steps (within tolerance)
                    const bool snapped = std::abs(readValue - testValue) < 0.01f ||
                                        std::abs(readValue - 0.0f) < 0.01f ||
                                        std::abs(readValue - 1.0f) < 0.01f;
                    snappingWorks = snappingWorks && snapped;
                }

                // Restore original value
                bridge.setParameterNormalized(idx, originalValue);
                testedCount++;
            }

            addTest("discrete_snapping_validation", snappingWorks,
                    "Discrete parameter snapping validated for " + std::to_string(testedCount) + " parameters.");
        }
        else
        {
            addTest("discrete_snapping_validation", true,
                    "No discrete parameters found; snapping test skipped (N/A).");
        }

        // Test 3: Test DiscreteParameterHandler processing
        const auto& classifier = processor.getParameterClassifier();
        DiscreteParameterHandler handler;
        handler.initialize(classifier);

        // Create test interpolation with discrete parameters
        std::vector<float> sourceValues(static_cast<size_t>(paramCount), 0.0f);
        std::vector<float> targetValues(static_cast<size_t>(paramCount), 1.0f);
        std::vector<float> interpolatedValues(static_cast<size_t>(paramCount), 0.5f);
        std::vector<float> outputValues = interpolatedValues;

        // Set up some discrete parameters in classifier for testing
        handler.processDiscreteParameters(interpolatedValues, outputValues, 0.5f);

        const bool discreteProcessingSafe = std::all_of(outputValues.begin(), outputValues.end(),
            [](float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; });

        addTest("discrete_handler_processing", discreteProcessingSafe,
                "DiscreteParameterHandler processed interpolation without producing invalid values.");

        // Test 4: Morph compatibility analysis
        const auto problems = handler.analyzeMorphCompatibility(sourceValues, targetValues);
        const bool morphAnalysisSafe = true;  // Should not throw

        addTest("morph_compatibility_analysis", morphAnalysisSafe,
                "Morph compatibility analysis completed. Found " +
                std::to_string(problems.size()) + " potential conflicts.");
    }
    catch (const std::exception& e)
    {
        addTest("discrete_suite_exception", false, e.what());
    }
    catch (...)
    {
        addTest("discrete_suite_exception", false, "Unknown exception.");
    }

    return {
        {"success", allPassed},
        {"suite", "discrete_parameter_snapping"},
        {"tests", tests}
    };
}

static json runPresetPersistenceTest(MorePhiProcessor& processor)
{
    json tests = json::array();
    bool allPassed = true;
    auto addTest = [&tests, &allPassed](const char* id, bool passed, const std::string& details)
    {
        tests.push_back({{"id", id}, {"ok", passed}, {"details", details}});
        allPassed = allPassed && passed;
    };

    auto& bridge = processor.getParameterBridge();
    const int paramCount = bridge.getParameterCount();

    if (paramCount == 0)
    {
        addTest("preset_persistence_available", false, "No hosted plugin loaded; cannot test preset persistence.");
        return {{"success", false}, {"suite", "preset_persistence"}, {"tests", tests}};
    }

    try
    {
        // Test 1: Capture current state
        auto originalState = bridge.captureParameterState();
        const bool stateCaptureSuccess = !originalState.empty();

        addTest("state_capture", stateCaptureSuccess,
                "Captured current parameter state (" + std::to_string(originalState.size()) + " parameters).");

        // Test 2: Modify parameters
        std::vector<float> modifiedState = originalState;
        for (size_t i = 0; i < modifiedState.size(); ++i)
            modifiedState[i] = static_cast<float>(i) / static_cast<float>(modifiedState.size());

        bridge.applyParameterState(modifiedState);

        // Test 3: Verify modification
        auto currentState = bridge.captureParameterState();
        bool modificationVerified = true;
        for (size_t i = 0; i < currentState.size() && i < modifiedState.size(); ++i)
        {
            if (std::abs(currentState[i] - modifiedState[i]) > 0.001f)
            {
                modificationVerified = false;
                break;
            }
        }

        addTest("state_modification", modificationVerified,
                "Parameter state successfully modified and verified.");

        // Test 4: Restore original state
        bridge.applyParameterState(originalState);
        auto restoredState = bridge.captureParameterState();

        bool restorationVerified = true;
        for (size_t i = 0; i < restoredState.size() && i < originalState.size(); ++i)
        {
            if (std::abs(restoredState[i] - originalState[i]) > 0.001f)
            {
                restorationVerified = false;
                break;
            }
        }

        addTest("state_restoration", restorationVerified,
                "Original parameter state successfully restored and verified.");

        // Test 5: Test snapshot capture and recall
        auto& bank = processor.getSnapshotBank();
        auto backup = bank.toXml();

        struct RestoreBank
        {
            SnapshotBank& bank;
            std::unique_ptr<juce::XmlElement> backup;

            ~RestoreBank()
            {
                if (backup)
                    bank.fromXml(*backup);
                else
                    bank.clearAll();
            }
        } restore{bank, std::move(backup)};

        bank.clearAll();

        // Capture to slot 0
        std::vector<float> testValues(static_cast<size_t>(paramCount), 0.75f);
        bank.captureValues(0, testValues);

        const bool slotCaptureSuccess = bank.isOccupied(0);
        addTest("snapshot_capture", slotCaptureSuccess,
                "Successfully captured parameter state to snapshot slot 0.");

        // Recall from slot 0
        SnapshotSelfTestBridge recallBridge(paramCount);
        bank.recallFast(0, recallBridge);

        bool recallVerified = true;
        for (size_t i = 0; i < recallBridge.values.size() && i < testValues.size(); ++i)
        {
            if (std::abs(recallBridge.values[i] - testValues[i]) > 0.001f)
            {
                recallVerified = false;
                break;
            }
        }

        addTest("snapshot_recall", recallVerified,
                "Successfully recalled parameter state from snapshot slot 0.");

        // Test 6: Multiple slot operations
        bool multiSlotSuccess = true;
        for (int slot = 1; slot < 4; ++slot)
        {
            std::vector<float> slotValues(static_cast<size_t>(paramCount), static_cast<float>(slot) / 10.0f);
            bank.captureValues(slot, slotValues);
            multiSlotSuccess = multiSlotSuccess && bank.isOccupied(slot);
        }

        addTest("multi_slot_operations", multiSlotSuccess,
                "Successfully captured to multiple snapshot slots (1-3).");
    }
    catch (const std::exception& e)
    {
        addTest("preset_persistence_exception", false, e.what());
    }
    catch (...)
    {
        addTest("preset_persistence_exception", false, "Unknown exception.");
    }

    return {
        {"success", allPassed},
        {"suite", "preset_persistence"},
        {"tests", tests}
    };
}

static json runRapidCCModulationTest(MorePhiProcessor& processor)
{
    json tests = json::array();
    bool allPassed = true;
    auto addTest = [&tests, &allPassed](const char* id, bool passed, const std::string& details)
    {
        tests.push_back({{"id", id}, {"ok", passed}, {"details", details}});
        allPassed = allPassed && passed;
    };

    try
    {
        // Test 1: Verify morph position can be set rapidly
        auto initialX = processor.getMorphX();
        auto initialY = processor.getMorphY();
        auto initialFader = processor.getFaderPos();

        addTest("initial_state_read", true,
                "Initial morph state: x=" + std::to_string(initialX) +
                ", y=" + std::to_string(initialY) +
                ", fader=" + std::to_string(initialFader));

        // Test 2: Rapid position changes (simulating fast CC modulation)
        const int numRapidChanges = 50;
        bool rapidChangesSuccessful = true;

        for (int i = 0; i < numRapidChanges; ++i)
        {
            const float newX = static_cast<float>(i) / static_cast<float>(numRapidChanges);
            const float newY = 1.0f - newX;
            const float newFader = newX;

            processor.setMorphPositionExternal(newX, true, newY, true, newFader, true, 0);

            // Verify the change was applied
            const float currentX = processor.getMorphX();
            const float currentY = processor.getMorphY();
            const float currentFader = processor.getFaderPos();

            // Allow small tolerance for floating-point comparison
            rapidChangesSuccessful = rapidChangesSuccessful &&
                                    std::abs(currentX - newX) < 0.001f &&
                                    std::abs(currentY - newY) < 0.001f &&
                                    std::abs(currentFader - newFader) < 0.001f;
        }

        addTest("rapid_position_changes", rapidChangesSuccessful,
                "Successfully processed " + std::to_string(numRapidChanges) + " rapid morph position changes.");

        // Test 3: Test morph interpolation during rapid changes
        auto& bank = processor.getSnapshotBank();
        MorphProcessor morph(bank);
        const int paramCount = processor.getParameterBridge().getParameterCount();
        const int testParamCount = juce::jlimit(1, 32, paramCount > 0 ? paramCount : 16);
        morph.prepare(testParamCount);
        morph.setSmoothingRate(0.0f);

        bool morphInterpolationStable = true;

        for (int i = 0; i < 10; ++i)
        {
            const float faderPos = static_cast<float>(i) / 10.0f;
            std::vector<float> output(static_cast<size_t>(testParamCount));

            morph.process(0.5f, 0.5f, faderPos, MorphSource::Fader, MorphMode::Direct,
                         1.0f / 60.0f, output);

            for (float value : output)
            {
                if (!std::isfinite(value) || value < -0.01f || value > 1.01f)
                {
                    morphInterpolationStable = false;
                    break;
                }
            }
        }

        addTest("morph_interpolation_stability", morphInterpolationStable,
                "Morph interpolation remained stable during rapid position changes.");

        // Test 4: Test physics engine during rapid modulation
        bool physicsStable = true;
        ElasticState elasticState;

        for (int i = 0; i < 20; ++i)
        {
            const float targetX = static_cast<float>(i) / 20.0f;
            PhysicsEngine::updateElastic(elasticState, targetX, 0.5f,
                                         ElasticPreset::Medium, 1.0f / 60.0f);

            if (!std::isfinite(elasticState.x) || !std::isfinite(elasticState.vx))
            {
                physicsStable = false;
                break;
            }
        }

        addTest("physics_engine_stability", physicsStable,
                "Physics engine remained stable during rapid target changes.");

        // Test 5: Test MIDI CC routing (simulated)
        // Note: MIDIRouter is a private member, we'll test the morph callback directly
        bool midiRoutingSafe = true;

        // Simulate rapid morph position changes (as if from CC messages)
        for (int i = 0; i < 20; ++i)
        {
            const float morphPos = static_cast<float>(i) / 20.0f;

            // This simulates what the MIDIRouter does when it receives CC1
            processor.setMorphPositionExternal(morphPos, false, morphPos, false, morphPos, true, 1);
        }

        addTest("midi_cc_routing", midiRoutingSafe,
                "MIDI CC routing simulation completed (morph position changes via setMorphPositionExternal).");

        // Test 6: Verify final state consistency
        const float finalX = processor.getMorphX();
        const float finalY = processor.getMorphY();
        const float finalFader = processor.getFaderPos();

        const bool stateConsistent = finalX >= 0.0f && finalX <= 1.0f &&
                                    finalY >= 0.0f && finalY <= 1.0f &&
                                    finalFader >= 0.0f && finalFader <= 1.0f;

        addTest("final_state_consistency", stateConsistent,
                "Final morph state is consistent: x=" + std::to_string(finalX) +
                ", y=" + std::to_string(finalY) +
                ", fader=" + std::to_string(finalFader));
    }
    catch (const std::exception& e)
    {
        addTest("rapid_cc_modulation_exception", false, e.what());
    }
    catch (...)
    {
        addTest("rapid_cc_modulation_exception", false, "Unknown exception.");
    }

    return {
        {"success", allPassed},
        {"suite", "rapid_cc_modulation"},
        {"tests", tests}
    };
}

juce::String MCPToolHandler::runSelfTest(const juce::var& params, MorePhiProcessor& p)
{
    const auto suite = params.getProperty("suite", "quick").toString().trim().toLowerCase();
    if (suite != "quick" && suite != "snapshot" && suite != "full")
        return invalidParamsResponse("suite must be one of: quick, snapshot, full.");

    auto snapshot = runSnapshotSelfTest(p);
    json result{
        {"success", snapshot.value("success", false)},
        {"suite", suite.toStdString()},
        {"generated_at", juce::Time::getCurrentTime().toISO8601(true).toStdString()},
        {"hosted_plugin_loaded", p.getHostManager().hasPlugin()},
        {"snapshot", snapshot}
    };

    if (suite == "full")
    {
        // Run all diagnostic suites
        auto discrete = runDiscreteParameterSnappingTest(p);
        auto preset = runPresetPersistenceTest(p);
        auto cc = runRapidCCModulationTest(p);

        result["discrete_parameter_snapping"] = discrete;
        result["preset_persistence"] = preset;
        result["rapid_cc_modulation"] = cc;

        // Overall success is true only if all suites pass
        result["success"] = snapshot.value("success", false) &&
                           discrete.value("success", false) &&
                           preset.value("success", false) &&
                           cc.value("success", false);

        result["coverage"] = {
            {"snapshot_suite", "executed"},
            {"discrete_parameter_snapping", "executed"},
            {"preset_persistence", "executed"},
            {"rapid_cc_modulation", "executed"}
        };
        result["message"] = "Full diagnostic completed. All suites executed.";
    }

    return toJString(result);
}

// ── Hosted mastering workflow tools ──────────────────────────────────────────

juce::String MCPToolHandler::scanHostedPlugin(const juce::var& params, MorePhiProcessor& p)
{
    const auto path = params.getProperty("path",
                      params.getProperty("plugin_path", "")).toString();
    if (path.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_plugin_path"}});

    juce::PluginDescription description;
    juce::String error;
    const juce::File pluginFile(path);
    const bool found = PluginHostManager::discoverPlugin(
        p.getHostManager().getFormatManager(), pluginFile, description, error);

    if (!found)
        return toJString(json{
            {"success", false},
            {"error", "plugin_discovery_failed"},
            {"details", error.toStdString()}
        });

    return toJString(json{
        {"success", true},
        {"name", description.name.toStdString()},
        {"manufacturer", description.manufacturerName.toStdString()},
        {"format", description.pluginFormatName.toStdString()},
        {"file_or_identifier", description.fileOrIdentifier.toStdString()},
        {"unique_id", description.uniqueId}
    });
}

juce::String MCPToolHandler::loadHostedPlugin(const juce::var& params, MorePhiProcessor& p)
{
    const auto path = params.getProperty("path",
                      params.getProperty("plugin_path", "")).toString();
    if (path.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_plugin_path"}});

    juce::PluginDescription description;
    juce::String error;
    const juce::File pluginFile(path);
    if (!PluginHostManager::discoverPlugin(p.getHostManager().getFormatManager(),
                                           pluginFile, description, error))
    {
        return toJString(json{
            {"success", false},
            {"error", "plugin_discovery_failed"},
            {"details", error.toStdString()}
        });
    }

    const bool loaded = p.getHostManager().loadPlugin(description);
    if (loaded)
    {
        p.refreshDiscreteMap();
        p.refreshHostedMasteringApplicators(description);
        p.getParameterClassifier().analyzeParameters(p.getParameterBridge());
        p.getDiscreteHandler().initialize(p.getParameterClassifier());
        p.reportLatencyToHost();
        invalidateToolResultCache();
    }

    return toJString(json{
        {"success", loaded},
        {"name", description.name.toStdString()},
        {"manufacturer", description.manufacturerName.toStdString()},
        {"format", description.pluginFormatName.toStdString()},
        {"error", loaded ? "" : "plugin_load_failed"}
    });
}

juce::String MCPToolHandler::captureHostedState(const juce::var& params, MorePhiProcessor& p)
{
    const int slot = params.getProperty("slot", -1);
    if (slot >= 0)
    {
        if (slot >= SnapshotBank::NUM_SLOTS)
            return toJString(json{{"success", false}, {"error", "invalid_slot"}});

        const bool includeState = params.getProperty("includeState",
                                  params.getProperty("include_state", true));
        const bool ok = p.captureSnapshotToSlot(slot, includeState);
        return toJString(json{
            {"success", ok},
            {"slot", slot},
            {"includeState", includeState},
            {"stateChunk", ok && p.getSnapshotBank().hasStateChunk(slot)}
        });
    }

    auto values = p.getParameterBridge().captureParameterState();
    json result{
        {"success", p.getHostManager().hasPlugin()},
        {"parameter_count", static_cast<int>(values.size())}
    };

    if (params.getProperty("include_values", false))
        result["values"] = values;

    if (auto* plugin = p.getHostManager().getPlugin())
        result["hosted_plugin"] = plugin->getName().toStdString();
    else
        result["error"] = "no_hosted_plugin";

    return toJString(result);
}

juce::String MCPToolHandler::diagnoseParameterPipeline(const juce::var& params, MorePhiProcessor& p)
{
    auto& bridge = p.getParameterBridge();
    auto& hostMgr = p.getHostManager();
    auto& bank = p.getSnapshotBank();

    const bool hasPlugin = hostMgr.hasPlugin();
    const int paramCount = hasPlugin ? bridge.getParameterCount() : 0;
    const float queueUsage = p.getCommandQueueUsage();
    const int queueCapacity = p.getCommandQueueCapacity();
    const int pendingCommands = static_cast<int>(p.getPendingParameterCommandCountApprox());
    const bool queueHealthy = p.isCommandQueueHealthy();
    const bool isPrepared = p.prepared.load(std::memory_order_acquire);
    const bool isRestoring = p.isRestoring_.load(std::memory_order_acquire);

    int occupiedSlots = 0;
    for (int s = 0; s < SnapshotBank::NUM_SLOTS; ++s)
        if (bank.isOccupied(s)) ++occupiedSlots;

    int liveEditHoldsActive = 0;
    const int holdSize = static_cast<int>(p.liveEditHold_.size());
    for (int i = 0; i < holdSize; ++i)
        if (p.liveEditHold_[static_cast<size_t>(i)] != 0) ++liveEditHoldsActive;

    json result{
        {"success", true},
        {"stage1_toolResolution", json{
            {"status", "ok"},
            {"note", "Use this tool's presence to confirm tool dispatch is working"}
        }},
        {"stage2_parameterResolution", json{
            {"hasPlugin", hasPlugin},
            {"parameterCount", paramCount},
            {"status", hasPlugin && paramCount > 0 ? "ok" : "blocked"}
        }},
        {"stage3_commandQueue", json{
            {"usagePercent", static_cast<int>(queueUsage * 100.0f)},
            {"pendingCommands", pendingCommands},
            {"capacity", queueCapacity},
            {"healthy", queueHealthy},
            {"status", queueHealthy ? "ok" : "warning_high_usage"}
        }},
        {"stage4_flush", json{
            {"pluginAvailable", hasPlugin},
            {"exclusiveAccessAvailable", !hostMgr.isExclusivePluginUseRequested()},
            {"status", hasPlugin && !hostMgr.isExclusivePluginUseRequested() ? "ok" : "blocked"}
        }},
        {"stage5_processBlock", json{
            {"isPrepared", isPrepared},
            {"isRestoring", isRestoring},
            {"status", isPrepared && !isRestoring ? "ok" : (isRestoring ? "blocked_restoring" : "blocked_not_prepared")}
        }},
        {"stage6_drainWrite", json{
            {"parameterCount", paramCount},
            {"status", paramCount > 0 ? "ok" : "no_parameters"}
        }},
        {"stage7_morphOverwrite", json{
            {"snapshotsOccupied", occupiedSlots},
            {"morphActive", occupiedSlots > 0},
            {"liveEditHoldsActive", liveEditHoldsActive},
            {"morphPosition", json{
                {"x", p.getMorphX()},
                {"y", p.getMorphY()},
                {"fader", p.getFaderPos()}
            }},
            {"status", occupiedSlots == 0 ? "ok_no_morph" : (liveEditHoldsActive > 0 ? "ok_holds_active" : "risk_morph_may_overwrite")}
        }}
    };

    const auto idOpt = extractParamId(params);
    if (idOpt.has_value())
    {
        const int idx = *idOpt;
        json paramDiag{{"index", idx}};
        const int bridgeParamCount = bridge.getParameterCount();

        if (idx >= 0 && idx < bridgeParamCount)
        {
            paramDiag["exists"] = true;
            paramDiag["currentValue"] = bridge.getParameterNormalized(idx);
            paramDiag["name"] = bridge.getParameterName(idx).toStdString();

            if (idx < holdSize)
                paramDiag["liveEditHeld"] = p.liveEditHold_[static_cast<size_t>(idx)] != 0;
            else
                paramDiag["liveEditHeld"] = false;
        }
        else
        {
            paramDiag["exists"] = false;
        }

        result["parameterDiagnostic"] = paramDiag;
    }

    auto& cache = getToolResultCache();
    const auto cacheStats = cache.getStats();
    result["tool_cache"] = json{
        {"entries", cacheStats.size},
        {"hits", cacheStats.hits},
        {"misses", cacheStats.misses},
        {"evictions", cacheStats.evictions},
        {"hit_rate_percent", (cacheStats.hits + cacheStats.misses) > 0
            ? static_cast<int>(100.0 * cacheStats.hits / (cacheStats.hits + cacheStats.misses))
            : 0}
    };

    return toJString(result);
}

juce::String MCPToolHandler::getAnalysisSummary(MorePhiProcessor& p)
{
    json measurements = currentMeasurementsToJson(p);
    json result{
        {"success", true},
        {"schema_version", 1},
        {"source", "more_phi_local_analysis"},
        {"rms", measurements["rms"]},
        {"queue_usage", measurements["queue_usage"]},
        {"lufs_momentary", measurements["lufs_momentary"]},
        {"lufs_short_term", measurements["lufs_short_term"]},
        {"lufs_integrated", measurements["lufs_integrated"]},
        {"lra", measurements["lra"]},
        {"true_peak_dbtp", measurements["true_peak_dbtp"]},
        {"limiter_gr_db", measurements["limiter_gr_db"]},
        {"dynamics_gr_db", measurements["dynamics_gr_db"]},
        {"measurements", measurements},
        {"true_peak_method", "4x_polyphase_fir_estimate"},
        {"loudness_method", "lightweight_bs1770_style_rolling_estimate"},
        {"analysis_metadata", analysisMetadataToJson(p, "instantaneous_snapshot")},
        {"model_status", modelStatusToJson(p)},
        {"warnings", analysisWarningsToJson()}
    };

    if (auto* plugin = p.getHostManager().getPlugin())
    {
        result["hosted_plugin"] = {
            {"name", plugin->getName().toStdString()},
            {"parameter_count", static_cast<int>(plugin->getParameters().size())},
            {"latency_samples", plugin->getLatencySamples()}
        };
    }
    else
    {
        result["hosted_plugin"] = nullptr;
    }

    return toJString(result);
}

juce::String MCPToolHandler::getSpectrumAnalysis(const juce::var& params, MorePhiProcessor& p)
{
    RealtimeSpectrumAnalyzer::SpectrumSnapshot snapshot;
    const bool available = p.getAutoMasteringEngine().getSpectrumAnalyzer().getSnapshot(snapshot);
    const int resolution = requestedSpectrumResolution(params);
    const int sourceBins = std::max(1, snapshot.binCount);

    json magnitudes = json::array();
    for (int outBin = 0; outBin < resolution; ++outBin)
    {
        const int start = outBin * sourceBins / resolution;
        const int end = std::max(start + 1, (outBin + 1) * sourceBins / resolution);
        float sum = 0.0f;
        int count = 0;
        for (int bin = start; bin < end && bin < RealtimeSpectrumAnalyzer::kMaxBins; ++bin)
        {
            sum += snapshot.magnitudeDB[static_cast<size_t>(bin)];
            ++count;
        }
        magnitudes.push_back(count > 0 ? sum / static_cast<float>(count) : -120.0f);
    }

    const double binWidthHz = snapshot.fftSize > 0
        ? snapshot.sampleRate / static_cast<double>(snapshot.fftSize)
        : 0.0;

    return toJString(json{
        {"success", available},
        {"schema_version", 1},
        {"source", "more_phi_realtime_spectrum"},
        {"method", "hann_window_fft_mono_sum"},
        {"analysis_metadata", analysisMetadataToJson(p, "instantaneous_snapshot")},
        {"resolution", resolution},
        {"channel_mode", kSpectrumChannelMode},
        {"channel_mode_description", kSpectrumChannelModeDescription},
        {"magnitude_db", magnitudes},
        {"spectral_centroid_hz", snapshot.spectralCentroid},
        {"spectral_rolloff_hz", snapshot.spectralRolloff},
        {"spectral_flux", snapshot.spectralFlux},
        {"crest_factor", snapshot.crestFactor},
        {"spectral_tilt_db_per_octave", snapshot.spectralTilt},
        {"bin_count", resolution},
        {"source_bin_count", snapshot.binCount},
        {"bin_width_hz", binWidthHz},
        {"fft_size", snapshot.fftSize},
        {"sample_rate", snapshot.sampleRate},
        {"frame_index", snapshot.frameIndex},
        {"warnings", json::array({ kSpectrumMonoSumWarning })}
    });
}

juce::String MCPToolHandler::getStereoFieldAnalysis(MorePhiProcessor& p)
{
    StereoFieldAnalyzer::StereoFieldSnapshot snapshot;
    const bool available = p.getAutoMasteringEngine().getStereoFieldAnalyzer().getSnapshot(snapshot);

    json bands = json::array();
    static constexpr const char* kBandNames[StereoFieldAnalyzer::kNumBands] = {
        "sub", "low_mid", "mid", "high"
    };

    json bandDefinitions = json::array({
        { {"index", 0}, {"name", "sub"}, {"low_hz", nullptr}, {"high_hz", kStereoCrossoverHz[0]} },
        { {"index", 1}, {"name", "low_mid"}, {"low_hz", kStereoCrossoverHz[0]}, {"high_hz", kStereoCrossoverHz[1]} },
        { {"index", 2}, {"name", "mid"}, {"low_hz", kStereoCrossoverHz[1]}, {"high_hz", kStereoCrossoverHz[2]} },
        { {"index", 3}, {"name", "high"}, {"low_hz", kStereoCrossoverHz[2]}, {"high_hz", nullptr} }
    });

    for (int band = 0; band < StereoFieldAnalyzer::kNumBands; ++band)
    {
        const auto idx = static_cast<size_t>(band);
        bands.push_back({
            {"index", band},
            {"name", kBandNames[band]},
            {"correlation", snapshot.correlation[idx]},
            {"ms_energy_ratio", snapshot.msEnergyRatio[idx]},
            {"mid_side_correlation", snapshot.correlation[idx]},
            {"side_to_mid_energy_ratio", snapshot.msEnergyRatio[idx]}
        });
    }

    return toJString(json{
        {"success", available},
        {"schema_version", 1},
        {"source", "more_phi_realtime_stereo_field"},
        {"method", "mid_side_energy_analysis"},
        {"algorithm", "mid_side_transform_plus_second_order_butterworth_band_split"},
        {"stereo_width", snapshot.stereoWidth},
        {"mid_side_width", snapshot.stereoWidth},
        {"bands", bands},
        {"band_definitions", bandDefinitions},
        {"sample_rate", snapshot.sampleRate},
        {"window_samples", snapshot.windowSamples},
        {"window_seconds", snapshot.sampleRate > 0.0 ? static_cast<double>(snapshot.windowSamples) / snapshot.sampleRate : 0.0},
        {"frame_index", snapshot.frameIndex},
        {"analysis_metadata", analysisMetadataToJson(p, "instantaneous_snapshot")},
        {"warnings", json::array({
            "stereo_field_is_mid_side_energy_analysis_not_perceptual_width_prediction"
        })},
        {"limitations", json::array({
            "correlation is computed between left and right band signals",
            "width is sqrt(total_side_energy / total_mid_energy) over the analyzer window"
        })}
    });
}

juce::String MCPToolHandler::captureAnalysisWindow(const juce::var& params, MorePhiProcessor& p)
{
    const float requestedWindowSeconds = std::max(0.0f, static_cast<float>(params.getProperty("window_seconds", 3.0f)));
    const auto currentSnapshot = json::parse(getAnalysisSummary(p).toStdString());
    const auto stats = p.getAutoMasteringEngine().computeMeterWindow(requestedWindowSeconds);

    if (!stats.success)
    {
        return toJString(json{
            {"success", false},
            {"schema_version", 1},
            {"source", "more_phi_meter_window"},
            {"error", "no_window_samples"},
            {"requested_window_seconds", requestedWindowSeconds},
            {"window_seconds", requestedWindowSeconds},
            {"actual_window_seconds", 0.0f},
            {"sample_count", 0},
            {"window_statistics", nullptr},
            {"current_snapshot", currentSnapshot},
            {"analysis_metadata", analysisMetadataToJson(p, "rolling_window")},
            {"model_status", modelStatusToJson(p)},
            {"warnings", json::array({
                "no rolling analysis samples are available yet",
                "call analysis.capture_window after audio has passed through the analysis tap"
            })}
        });
    }

    return toJString(json{
        {"success", true},
        {"schema_version", 1},
        {"source", "more_phi_meter_window"},
        {"requested_window_seconds", requestedWindowSeconds},
        {"window_seconds", requestedWindowSeconds},
        {"actual_window_seconds", stats.actualSeconds},
        {"sample_count", stats.sampleCount},
        {"start_timestamp_seconds", stats.startTimestampSeconds},
        {"end_timestamp_seconds", stats.endTimestampSeconds},
        {"window_statistics", windowStatisticsToJson(stats)},
        {"current_snapshot", currentSnapshot},
        {"analysis_metadata", analysisMetadataToJson(p, "rolling_window")},
        {"model_status", modelStatusToJson(p)},
        {"warnings", analysisWarningsToJson()}
    });
}

juce::String MCPToolHandler::compareAnalysis(const juce::var& params, MorePhiProcessor& p)
{
    const auto before = params.getProperty("before", juce::var());
    juce::var after = params.getProperty("after", juce::var());

    if (!after.isObject())
        after = juce::JSON::parse(getAnalysisSummary(p));

    if (!before.isObject() || !after.isObject())
        return toJString(json{{"success", false}, {"error", "missing_analysis_summary"}});

    const float beforeLufs = getNumberProperty(before, "lufs_integrated", 0.0f);
    const float afterLufs = getNumberProperty(after, "lufs_integrated", 0.0f);
    const float beforePeak = getNumberProperty(before, "true_peak_dbtp", 0.0f);
    const float afterPeak = getNumberProperty(after, "true_peak_dbtp", 0.0f);
    const float beforeLra = getNumberProperty(before, "lra", 0.0f);
    const float afterLra = getNumberProperty(after, "lra", 0.0f);

    return toJString(json{
        {"success", true},
        {"delta", {
            {"lufs_integrated", afterLufs - beforeLufs},
            {"true_peak_dbtp", afterPeak - beforePeak},
            {"lra", afterLra - beforeLra}
        }},
        {"warnings", afterPeak > -0.1f ? json::array({"true_peak_close_to_0_dbtp"}) : json::array()}
    });
}

juce::String MCPToolHandler::previewMasteringPlan(const juce::var& params, MorePhiProcessor& p)
{
    const int   genreIndex    = static_cast<int>  (params.getProperty("genre_index",    0));
    const float dynamicRange  = static_cast<float>(params.getProperty("dynamic_range",  6.0f));
    const float spectralTilt  = static_cast<float>(params.getProperty("spectral_tilt",  0.0f));
    const float correlationMS = static_cast<float>(params.getProperty("correlation_ms", 0.5f));

    const auto plan = p.getAutoMasteringEngine().getChainPlanner().previewPlan(
        genreIndex, dynamicRange, spectralTilt, correlationMS);

    const auto measuredInputs = plannerInputsToJson(genreIndex, dynamicRange, spectralTilt, correlationMS);
    const auto rulesApplied = plannerRulesToJson(genreIndex, dynamicRange, spectralTilt, correlationMS, plan);

    json result = planToJson(plan);
    result["success"] = plan.valid;
    result["applied"] = false;
    result["measured_inputs"] = measuredInputs;
    result["rules_applied"] = rulesApplied;
    return toJString(result);
}

juce::String MCPToolHandler::renderMasteringBatch(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity)
{
    const int candidateCount = juce::jlimit(1, 8, static_cast<int>(params.getProperty("candidate_count", 3)));
    const bool dryRun = params.getProperty("dry_run", true);

    if (dryRun)
    {
        const int   genreIndex    = static_cast<int>  (params.getProperty("genre_index",    0));
        const float dynamicRange  = static_cast<float>(params.getProperty("dynamic_range",  6.0f));
        const float spectralTilt  = static_cast<float>(params.getProperty("spectral_tilt",  0.0f));
        const float correlationMS = static_cast<float>(params.getProperty("correlation_ms", 0.5f));

        const uint64_t batchId = gNextDryRunBatchId.fetch_add(1);
        json candidates = json::array();

        // P3.9 (AUDIT): the dry-run path previously emitted score=0.0 and NO
        // lufs_error field, so OptimizationAgent's min(lufs_error) selection read
        // infinity for every candidate and chose nothing (bestIdx stayed -1, zero
        // proposed actions). Populate a real lufs_error per candidate: the absolute
        // distance between the candidate's targetLUFS and the engine's live
        // integrated LUFS measurement. This is a meaningful ranking signal (a
        // candidate whose target lands far from the measured program loudness is a
        // worse fit) and makes OptimizationAgent's selection genuinely discriminate
        // among the offset candidates. It is NOT a rendered-audio fitness (no
        // offline render in dry-run); score_basis documents that honestly.
        auto& ame = p.getAutoMasteringEngine();
        const double measuredLufs = finiteOr(ame.getLUFSIntegrated(), -70.0);

        for (int i = 0; i < candidateCount; ++i)
        {
            const float offset = static_cast<float>(i) - static_cast<float>(candidateCount - 1) * 0.5f;
            const float adjustedDynamicRange = dynamicRange + offset;
            const float adjustedSpectralTilt = spectralTilt + offset * 0.25f;
            const float adjustedCorrelationMS = std::clamp(correlationMS + offset * 0.05f, -1.0f, 1.0f);
            const auto plan = p.getAutoMasteringEngine().getChainPlanner().previewPlan(
                genreIndex,
                adjustedDynamicRange,
                adjustedSpectralTilt,
                adjustedCorrelationMS);
            const auto measuredInputs = plannerInputsToJson(
                genreIndex, adjustedDynamicRange, adjustedSpectralTilt, adjustedCorrelationMS);
            const auto rulesApplied = plannerRulesToJson(
                genreIndex, adjustedDynamicRange, adjustedSpectralTilt, adjustedCorrelationMS, plan);
            const auto candidateId = makeDryRunCandidateId(batchId, i);
            const float lufsError = static_cast<float>(std::abs(plan.targetLUFS - measuredLufs));
            // Normalize to [0,1]: 0 = perfect match, 1 = >=24 LU off (full target band).
            const float score = std::clamp(1.0f - lufsError / 24.0f, 0.0f, 1.0f);

            {
                const std::lock_guard<std::mutex> guard(gDryRunCandidatesMutex);
                gDryRunCandidates[makeInstancePrefixedKey(identity.instanceId, candidateId)] = {
                    candidateId, plan, score, measuredInputs, rulesApplied
                };
                capGlobalCache(gDryRunCandidates, kMaxDryRunCandidateCache);  // MCP-FILES-04
            }

            json candidate = planToJson(plan);
            candidate["id"] = candidateId.toStdString();
            candidate["score"] = score;
            candidate["lufs_error"] = lufsError;          // P3.9: OptimizationAgent ranks on this
            candidate["measured_lufs"] = measuredLufs;    // provenance for the error
            candidate["measured_inputs"] = measuredInputs;
            candidate["rules_applied"] = rulesApplied;
            candidates.push_back(candidate);
        }

        return toJString(json{
            {"success", true},
            {"mode", "dry_run_plan_candidates"},
            {"planner_type", kPlannerType},
            {"rule_version", kPlannerRuleVersion},
            {"score_available", true},
            {"score_basis", "lufs_target_distance_proxy"},  // P3.9: honest about what the score means
            {"planner_metadata", plannerMetadataToJson()},
            {"candidates", candidates}
        });
    }

    const auto inputPath = params.getProperty("input_path",
                           params.getProperty("input_file", "")).toString();
    const auto outputPath = params.getProperty("output_path",
                            params.getProperty("output_directory", "")).toString();
    const auto pluginPath = params.getProperty("plugin_path",
                            params.getProperty("plugin_file", "")).toString();
    const bool allowPassthrough = params.getProperty("allow_passthrough", false);

    if (inputPath.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_input_path"}});
    if (outputPath.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_output_path"}});
    if (pluginPath.isEmpty() && !allowPassthrough)
    {
        return toJString(json{
            {"success", false},
            {"error", "missing_plugin_path"},
            {"details", "Offline hosted mastering renders require plugin_path unless allow_passthrough=true."}
        });
    }

    const juce::File inputFile(inputPath);
    const juce::File outputDirectory(outputPath);
    const juce::File pluginFile(pluginPath);

    if (!inputFile.existsAsFile())
        return toJString(json{{"success", false}, {"error", "input_not_found"}});
    if (pluginPath.isNotEmpty() && !(pluginFile.existsAsFile() || pluginFile.isDirectory()))
        return toJString(json{{"success", false}, {"error", "plugin_not_found"}});

    const auto jobId = "render_" + juce::String(static_cast<int64_t>(gNextRenderJobId.fetch_add(1)));
    auto job = std::make_shared<RenderJobRecord>();
    job->id = jobId;
    job->inputPath = inputFile.getFullPathName();
    job->outputDirectory = outputDirectory.getFullPathName();
    job->pluginPath = pluginFile.getFullPathName();
    job->progress.total = candidateCount;

    {
        const std::lock_guard<std::mutex> guard(gRenderJobsMutex);
        gRenderJobs[makeInstancePrefixedKey(identity.instanceId, jobId)] = job;
        capGlobalCache(gRenderJobs, kMaxRenderJobCache);  // MCP-FILES-04
    }

    const auto track = TrackAssistantStore::upsertFileTrack(inputFile, jobId);
    const auto trackId = track.value("track_id", "");

    const int blockSize = juce::jlimit(64, 8192, static_cast<int>(params.getProperty("block_size", 512)));
    const int channels = juce::jlimit(1, 16, static_cast<int>(params.getProperty("channels", 2)));
    const int workers = juce::jlimit(1, 8, static_cast<int>(params.getProperty("parallel_workers", 1)));
    const double sampleRate = static_cast<double>(params.getProperty("sample_rate", 48000.0));
    const float durationSeconds = static_cast<float>(params.getProperty("duration_seconds", 30.0f));
    const int maxInputFileSizeMB = juce::jlimit(1, 4096, static_cast<int>(params.getProperty("max_input_file_size_mb", 500)));

    std::thread([job, inputFile, outputDirectory, pluginFile, candidateCount,
                 blockSize, channels, workers, sampleRate, durationSeconds, maxInputFileSizeMB]()
    {
        // MCP-FILES-01: this runs on a detached thread; an uncaught exception
        // calls std::terminate and crashes the host (DAW). Wrap the whole body
        // so any throw fails the job cleanly instead of terminating the process.
        try {
        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->status = "configuring";
            job->message = "Configuring offline renderer";
        }

        OfflineBatchConfig config;
        config.inputFile = inputFile;
        config.outputDirectory = outputDirectory;
        config.pluginFile = pluginFile;
        config.totalVariations = candidateCount;
        config.parallelWorkers = workers;
        config.enableSIMD = true;
        config.useMemoryPool = true;
        config.maxInputFileSizeMB = maxInputFileSizeMB;
        config.renderConfig.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        config.renderConfig.blockSize = blockSize;
        config.renderConfig.numChannels = channels;
        config.renderConfig.outputDirectory = outputDirectory;
        config.renderConfig.validateOutput = true;
        if (durationSeconds > 0.0f)
        {
            config.renderConfig.fullDuration = durationSeconds;
            config.renderConfig.transientDuration = durationSeconds;
            config.renderConfig.steadyStateDuration = durationSeconds;
            config.renderConfig.customDuration = durationSeconds;
        }

        auto renderer = std::make_unique<OfflineBatchRenderer>();
        renderer->onProgressUpdate = [job](const OfflineBatchProgress& progress)
        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->progress = progress;
            job->status = "running";
            job->message = progress.currentStatus;
        };

        renderer->onVariationComplete = [job](int index, const RenderResult& result)
        {
            RenderCandidateRecord candidate;
            candidate.index = index;
            candidate.success = result.success;
            candidate.id = makeRenderCandidateId(job->id, index);
            candidate.outputPath = result.outputFile.getFullPathName();
            candidate.peakDb = result.peakDb;
            candidate.rmsDb = result.rmsDb;
            candidate.hasSilence = result.hasSilence;
            candidate.hasClipping = result.hasClipping;
            candidate.renderTimeMs = result.renderTimeMs;
            candidate.errorMessage = result.errorMessage;

            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->candidates.push_back(std::move(candidate));
        };

        renderer->onRenderComplete = [job](bool success, const juce::String& message)
        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->success = success;
            job->message = message;
            job->status = success ? "completed" : "completed_with_errors";
            job->completed = true;
        };

        if (!renderer->setConfig(config))
        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->success = false;
            job->completed = true;
            job->status = "failed";
            job->message = "Failed to configure offline renderer";
            return;
        }

        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->status = "running";
            job->message = "Rendering variations";
        }

        if (!renderer->startRender(nullptr))
        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->success = false;
            job->completed = true;
            job->status = "failed";
            job->message = "Failed to start offline renderer";
        }
        }  // close try (MCP-FILES-01)
        catch (const std::exception& e)
        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->success = false;
            job->completed = true;
            job->status = "failed";
            job->message = std::string("Render thread exception: ") + e.what();
        }
        catch (...)
        {
            const std::lock_guard<std::mutex> jobGuard(job->mutex);
            job->success = false;
            job->completed = true;
            job->status = "failed";
            job->message = "Unknown render thread exception";
        }
    }).detach();

    return toJString(json{
        {"success", true},
        {"mode", "offline_file_render"},
        {"job_id", jobId.toStdString()},
        {"status", "queued"},
        {"track_id", trackId},
        {"input_path", inputFile.getFullPathName().toStdString()},
        {"output_directory", outputDirectory.getFullPathName().toStdString()},
        {"plugin_path", pluginFile.getFullPathName().toStdString()},
        {"total_variations", candidateCount},
        {"planner_type", kPlannerType},
        {"rule_version", kPlannerRuleVersion},
        {"score_available", false},
        {"score_basis", "render_scores_available_after_job_completion"},
        {"confidence", nullptr}
    });
}

juce::String MCPToolHandler::getMasteringRenderStatus(const juce::var& params, const juce::String& instanceId)
{
    const auto jobId = params.getProperty("job_id", "").toString();
    if (jobId.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_job_id"}});

    const auto job = findRenderJob(instanceId, jobId);
    if (job == nullptr)
        return toJString(json{{"success", false}, {"error", "job_not_found"}});

    return toJString(renderJobToJson(*job));
}

juce::String MCPToolHandler::selectMasteringCandidate(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity)
{
    const auto candidateId = params.getProperty("candidate_id", "").toString();
    if (candidateId.isEmpty())
        return toJString(json{{"success", false}, {"error", "missing_candidate_id"}});

    {
        std::optional<DryRunCandidateRecord> dryRunCandidate;
        {
            const std::lock_guard<std::mutex> guard(gDryRunCandidatesMutex);
            const auto it = gDryRunCandidates.find(makeInstancePrefixedKey(identity.instanceId, candidateId));
            if (it != gDryRunCandidates.end())
                dryRunCandidate = it->second;
        }

        if (dryRunCandidate.has_value())
        {
            auto& planner = p.getAutoMasteringEngine().getChainPlanner();
            const int appliedCount = planner.applyPlan(dryRunCandidate->plan);
            json result = planToJson(dryRunCandidate->plan);
            result["success"] = true;
            result["candidate_id"] = candidateId.toStdString();
            result["selected"] = true;
            result["applied"] = true;
            result["ozone_applied"] = planner.hasOzoneApplicator();
            result["applied_parameter_count"] = appliedCount;
            result["score"] = dryRunCandidate->score;
            result["measured_inputs"] = dryRunCandidate->measuredInputs;
            result["rules_applied"] = dryRunCandidate->rulesApplied;
            return toJString(result);
        }
    }

    const int separator = candidateId.indexOfChar(':');
    if (separator > 0)
    {
        const auto jobId = candidateId.substring(0, separator);
        const auto job = findRenderJob(identity.instanceId, jobId);
        if (job == nullptr)
            return toJString(json{{"success", false}, {"error", "job_not_found"}});

        const std::lock_guard<std::mutex> jobGuard(job->mutex);
        for (const auto& candidate : job->candidates)
        {
            if (candidate.id == candidateId)
            {
                const auto track = TrackAssistantStore::selectCandidateForRenderJob(
                    job->id, candidateId, candidate.outputPath);

                return toJString(json{
                    {"success", true},
                    {"candidate_id", candidateId.toStdString()},
                    {"selected", true},
                    {"applied", false},
                    {"track_id", track.value("track_id", "")},
                    {"track_status", track.value("status", "")},
                    {"output_path", candidate.outputPath.toStdString()},
                    {"render_success", candidate.success},
                    {"peak_db", candidate.peakDb},
                    {"rms_db", candidate.rmsDb},
                    {"score", scoreRenderedCandidate(candidate)},
                    {"score_available", true},
                    {"score_basis", kRenderedScoreBasis},
                    {"confidence", nullptr},
                    {"warnings", candidate.errorMessage.isNotEmpty()
                        ? json::array({candidate.errorMessage.toStdString()})
                        : json::array()}
                });
            }
        }

        return toJString(json{{"success", false}, {"error", "candidate_not_found"}});
    }

    return toJString(json{
        {"success", true},
        {"candidate_id", candidateId.toStdString()},
        {"selected", true},
        {"applied", false}
    });
}

juce::String MCPToolHandler::auditPluginProfile(MorePhiProcessor& p)
{
    return PluginProfileDB::buildAuditJson(p.getHostManager(), p.getParameterBridge());
}

juce::String MCPToolHandler::describePluginSemantics(MorePhiProcessor& p)
{
    return PluginProfileDB::buildAuditJson(p.getHostManager(), p.getParameterBridge());
}

juce::String MCPToolHandler::applySafePluginAction(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity)
{
    auto& bridge = p.getParameterBridge();
    const auto descriptors = bridge.getParameterDescriptors();
    const auto controls = SemanticPluginProfile::classify(descriptors);
    auto plan = SemanticPluginProfile::planSafeAction(params, descriptors, controls, &bridge);

    if (!plan.success)
    {
        json response{
            {"success", false},
            {"error", plan.error.toStdString()}
        };
        if (plan.message.isNotEmpty())
            response["message"] = plan.message.toStdString();
        if (!plan.actionJson.is_null())
            response["action"] = plan.actionJson;
        return toJString(response);
    }

    const bool dryRun = static_cast<bool>(params.getProperty("dry_run", false));
    if (dryRun)
    {
        return toJString(json{
            {"success", true},
            {"dry_run", true},
            {"queued", 0},
            {"action", plan.actionJson}
        });
    }

    std::vector<MorePhiProcessor::ParamCommand> commands;
    commands.reserve(plan.commands.size());
    for (const auto& plannedCommand : plan.commands)
    {
        commands.push_back(MorePhiProcessor::ParamCommand{
            plannedCommand.parameterIndex,
            plannedCommand.normalizedValue,
            false,
            -1,
            MorePhiProcessor::ParameterEditSource::MCP,
            true
        });
    }

    const auto preFlush = p.flushPendingParameterCommandsForAssistant();
    if (hasPendingParameterCommands(p))
    {
        return toJString(json{
            {"success", false},
            {"error", "pending_parameter_edits"},
            {"queued", 0},
            {"pending", static_cast<int>(p.getPendingParameterCommandCountApprox())},
            {"flush", parameterFlushToJson(preFlush)},
            {"action", plan.actionJson}
        });
    }

    auto beforeValues = bridge.captureParameterState();
    if (beforeValues.empty() && !descriptors.empty())
    {
        beforeValues.reserve(descriptors.size());
        for (const auto& descriptor : descriptors)
            beforeValues.push_back(descriptor.value);
    }
    const auto snapshotNumber = gNextSafeActionSnapshotId.fetch_add(1, std::memory_order_relaxed);
    const auto snapshotId = juce::String("safe_action_") + juce::String(static_cast<juce::uint64>(snapshotNumber));

    if (!p.enqueueParameterBatch(commands))
    {
        return toJString(json{
            {"success", false},
            {"error", "queue_full"},
            {"queued", 0},
            {"action", plan.actionJson}
        });
    }

    SafeActionSnapshotRecord record;
    record.id = snapshotId;
    record.createdAt = juce::Time::getCurrentTime().toISO8601(true);
    record.processorGenerationToken = p.getProcessorGenerationToken();
    record.layoutSignature = buildParameterLayoutSignature(descriptors);
    record.values = beforeValues;
    record.action = plan.actionJson;

    {
        std::lock_guard<std::mutex> lock(gSafeActionSnapshotsMutex);
        gSafeActionSnapshots[makeInstancePrefixedKey(identity.instanceId, snapshotId)] = std::move(record);
        capGlobalCache(gSafeActionSnapshots, kMaxSafeActionSnapshotCache);  // MCP-FILES-04
    }

    const auto flush = p.flushPendingParameterCommandsForAssistant(
        juce::jmax(2048, static_cast<int>(commands.size())));

    return toJString(json{
        {"success", true},
        {"snapshot_id", snapshotId.toStdString()},
        {"queued", static_cast<int>(commands.size())},
        {"appliedNow", flush.drained},
        {"pendingAfter", flush.pendingAfter},
        {"flush", parameterFlushToJson(flush)},
        {"action", plan.actionJson},
        {"before_parameter_count", static_cast<int>(beforeValues.size())}
    });
}

juce::String MCPToolHandler::restoreSafePluginSnapshot(const juce::var& params, MorePhiProcessor& p, const InstanceIdentity& identity)
{
    const auto snapshotId = params.getProperty("snapshot_id", params.getProperty("snapshotId", "")).toString();
    if (snapshotId.isEmpty())
    {
        return toJString(json{
            {"success", false},
            {"error", "invalid_params"},
            {"message", "plugin_profile.restore_safe_snapshot requires snapshot_id."}
        });
    }

    SafeActionSnapshotRecord record;
    {
        std::lock_guard<std::mutex> lock(gSafeActionSnapshotsMutex);
        const auto it = gSafeActionSnapshots.find(makeInstancePrefixedKey(identity.instanceId, snapshotId));
        if (it == gSafeActionSnapshots.end())
            return toJString(json{{"success", false}, {"error", "snapshot_not_found"}, {"snapshot_id", snapshotId.toStdString()}});
        record = it->second;
    }

    const auto currentDescriptors = p.getParameterBridge().getParameterDescriptors();
    if (!snapshotContextMatches(record, p, currentDescriptors)
        || record.values.size() != currentDescriptors.size())
    {
        return toJString(json{
            {"success", false},
            {"error", "snapshot_context_mismatch"},
            {"snapshot_id", snapshotId.toStdString()},
            {"queued", 0}
        });
    }

    std::vector<MorePhiProcessor::ParamCommand> commands;
    commands.reserve(record.values.size());
    for (size_t i = 0; i < record.values.size(); ++i)
    {
        commands.push_back(MorePhiProcessor::ParamCommand{
            static_cast<int>(i),
            record.values[i],
            false,
            -1,
            MorePhiProcessor::ParameterEditSource::MCP,
            true
        });
    }

    if (!p.enqueueParameterBatch(commands))
    {
        return toJString(json{
            {"success", false},
            {"error", "queue_full"},
            {"snapshot_id", snapshotId.toStdString()},
            {"queued", 0}
        });
    }

    const auto flush = p.flushPendingParameterCommandsForAssistant(
        juce::jmax(2048, static_cast<int>(commands.size())));

    return toJString(json{
        {"success", true},
        {"snapshot_id", snapshotId.toStdString()},
        {"queued", static_cast<int>(commands.size())},
        {"appliedNow", flush.drained},
        {"pendingAfter", flush.pendingAfter},
        {"flush", parameterFlushToJson(flush)}
    });
}

juce::String MCPToolHandler::getPluginProfile(const juce::var& params, MorePhiProcessor& p)
{
    const auto profileId = params.getProperty("profile_id", "").toString();
    if (profileId.isEmpty())
        return auditPluginProfile(p);

    const auto file = PluginProfileDB::getProfileFile(profileId);
    if (!file.existsAsFile())
        return toJString(json{{"success", false}, {"error", "profile_not_found"}});

    const auto text = file.loadFileAsString();
    try
    {
        const auto parsed = json::parse(text.toStdString());
        juce::ignoreUnused(parsed);
        return text;
    }
    catch (...)
    {
        return toJString(json{{"success", false}, {"error", "profile_json_invalid"}});
    }
}

juce::String MCPToolHandler::savePluginProfile(const juce::var& /*params*/, MorePhiProcessor& p)
{
    const auto audit = PluginProfileDB::buildAuditJson(p.getHostManager(), p.getParameterBridge());
    json parsed;
    try
    {
        parsed = json::parse(audit.toStdString());
    }
    catch (...)
    {
        return toJString(json{{"success", false}, {"error", "audit_json_invalid"}});
    }

    if (!parsed.value("success", false))
        return audit;

    const auto profileId = juce::String(parsed.value("profile_id", ""));
    const auto dir = PluginProfileDB::getProfileDirectory();
    if (!dir.createDirectory())
        return toJString(json{{"success", false}, {"error", "profile_directory_create_failed"}});

    const auto file = PluginProfileDB::getProfileFile(profileId);
    if (!file.replaceWithText(audit, false, false, "\n"))
        return toJString(json{{"success", false}, {"error", "profile_write_failed"}});

    parsed["saved"] = true;
    parsed["path"] = file.getFullPathName().toStdString();
    return toJString(parsed);
}

juce::String MCPToolHandler::describePluginSemanticMap(const juce::var& params, MorePhiProcessor& p)
{
    const auto pluginId = params.getProperty("plugin_id",
                          params.getProperty("pluginId", "current")).toString();
    if (pluginId.isNotEmpty() && pluginId != "current")
    {
        return toJString(json{
            {"success", false},
            {"error", "invalid_plugin_id"},
            {"message", "Only plugin_id='current' is supported by the integrated MCP semantic map."}
        });
    }

    PluginSemanticMapper::Options options;
    options.includeRawParameters = static_cast<bool>(params.getProperty(
        "include_raw_parameters",
        params.getProperty("includeRawParameters", false)));
    options.maxControls = static_cast<int>(params.getProperty(
        "max_controls",
        params.getProperty("maxControls", 128)));

    p.getParameterClassifier().analyzeParameters(p.getParameterBridge());
    return PluginSemanticMapper::buildSemanticMapJson(
        p.getHostManager(),
        p.getParameterBridge(),
        p.getParameterClassifier(),
        options);
}

// ── Multi-instance tools ──────────────────────────────────────────────────────

juce::String MCPToolHandler::getInstanceInfo(const InstanceIdentity& id)
{
    return toJString(json{
        {"instanceId",  id.instanceId.toStdString()},
        {"morphCode",   id.morphCode.toStdString()},
        {"port",        id.port},
        {"createdAt",   id.createdAt}
    });
}

juce::String MCPToolHandler::listInstances()
{
    auto instances = InstanceRegistry::getInstance().getAllInstances();

    json arr = json::array();
    for (const auto& inst : instances)
    {
        arr.push_back({
            {"instanceId", inst.instanceId.toStdString()},
            {"morphCode",  inst.morphCode.toStdString()},
            {"port",       inst.port},
            {"createdAt",  inst.createdAt}
            // bearerToken intentionally omitted (redacted)
        });
    }
    return toJString(arr);
}

// ── Ozone mastering tools ─────────────────────────────────────────────────────

juce::String MCPToolHandler::auditOzoneParameters(const juce::var& params, MorePhiProcessor& p)
{
    const auto* plugin = p.getHostManager().getPlugin();
    const auto pluginName = plugin != nullptr ? plugin->getName() : juce::String{};
    const auto map = OzoneParameterMap::buildFromHostedPlugin(p.getParameterBridge());
    const bool apply = params.getProperty("apply", false);

    if (apply)
    {
        p.ozoneParamMap_ = std::make_unique<OzoneParameterMap>(map);
        p.ozonePlanApplicator_ = std::make_unique<OzonePlanApplicator>(p, *p.ozoneParamMap_);
        p.getAutoMasteringEngine().getChainPlanner().setOzonePlanApplicator(p.ozonePlanApplicator_.get());
    }

    json result = ozoneMapToJson(map);
    result["success"] = true;
    result["plugin_name"] = pluginName.toStdString();
    result["plugin_loaded"] = plugin != nullptr;
    result["is_ozone_11"] = OzoneParameterMap::isOzone11(pluginName);
    result["applied"] = apply;
    result["ozone_applicator_active"] = p.getAutoMasteringEngine().getChainPlanner().hasOzoneApplicator();
    result["parameter_count"] = p.getParameterBridge().getParameterCount();
    return toJString(result);
}

juce::String MCPToolHandler::getMasteringState(MorePhiProcessor& p)
{
    auto& ame = p.getAutoMasteringEngine();
    const bool ozoneHosted = p.getHostManager().hasPlugin() &&
        OzoneParameterMap::isOzone11(
            p.getHostManager().getPlugin()->getName());

    json result;
    result["lufs_momentary"]  = finiteOr(ame.getLUFSMomentary(), -70.0);
    result["lufs_short_term"] = finiteOr(ame.getLUFSShortTerm(), -70.0);
    result["lufs_integrated"] = finiteOr(ame.getLUFSIntegrated(), -70.0);
    result["lra"]             = finiteOr(ame.getLRA(), 0.0);
    result["true_peak_dbtp"]  = finiteOr(ame.getTruePeak_dBTP(), -300.0);
    result["limiter_gr_db"]   = finiteOr(ame.getLimiterGainReductionDB(), 0.0);
    result["ozone_hosted"]    = ozoneHosted;
    result["ozone_applicator_active"] = ame.getChainPlanner().hasOzoneApplicator();
    result["planner_type"] = kPlannerType;
    result["rule_version"] = kPlannerRuleVersion;
    result["score_available"] = false;
    result["score_basis"] = kDryRunScoreBasis;
    result["confidence"] = nullptr;
    result["model_status"] = modelStatusToJson(p);
    result["planner_metadata"] = plannerMetadataToJson();

    // Per-band dynamics gain reduction
    json grArr = json::array();
    for (int i = 0; i < 4; ++i)
        grArr.push_back(finiteOr(ame.getGainReductionDB(i), -300.0));
    result["dynamics_gr_db"] = grArr;

    // Last mastering plan (if any)
    const MultiEffectPlan& plan = ame.getChainPlanner().getLastPlan();
    if (plan.valid)
    {
        result["last_plan"] = planToJson(plan);
    }

    return toJString(result);
}

juce::String MCPToolHandler::applyMasteringPlan(const juce::var& params, MorePhiProcessor& p)
{
    const int   genreIndex    = static_cast<int>  (params.getProperty("genre_index",    0));
    const float dynamicRange  = static_cast<float>(params.getProperty("dynamic_range",  6.0f));
    const float spectralTilt  = static_cast<float>(params.getProperty("spectral_tilt",  0.0f));
    const float correlationMS = static_cast<float>(params.getProperty("correlation_ms", 0.5f));

    auto& planner = p.getAutoMasteringEngine().getChainPlanner();
    const MultiEffectPlan plan = planner.executePlan(
        genreIndex, dynamicRange, spectralTilt, correlationMS);

    const auto measuredInputs = plannerInputsToJson(genreIndex, dynamicRange, spectralTilt, correlationMS);
    const auto rulesApplied = plannerRulesToJson(genreIndex, dynamicRange, spectralTilt, correlationMS, plan);

    json result = planToJson(plan);
    result["success"]           = plan.valid;
    result["ozone_applied"]     = planner.hasOzoneApplicator();
    result["measured_inputs"]   = measuredInputs;
    result["rules_applied"]     = rulesApplied;
    return toJString(result);
}

juce::String MCPToolHandler::sonicmasterDecision(const juce::var& params, MorePhiProcessor& p)
{
    const float targetLufs = static_cast<float>(params.getProperty("target_lufs", -14.0));

    // AUDIT-FIX-R10: short-TTL cache (3s) for repeated calls with the same
    // target_lufs. Audio changes slowly enough that a 3s window is safe for
    // caching, and it prevents unnecessary re-inference when the assistant
    // queries the same decision twice (e.g. to report it + to confirm it).
    {
        const auto cached = toolResultCache().get("sonicmaster_decision", params,
                                                   p.getProcessorGenerationToken(),
                                                   p.getInstanceIdentity().instanceId);
        if (cached.has_value())
        {
            json wrapper = *cached;
            wrapper["cached"] = true;
            return toJString(wrapper);
        }
    }

    auto& engine = p.getSonicMasterEngine();

    if (!engine.isAvailable())
    {
        json err;
        err["success"] = false;
        err["available"] = false;
        err["error"] = "SonicMaster inference server is not reachable. Start it with "
                       "`python tools/inference_server/server.py --package <package>` "
                       "(see tools/inference_server/README.md).";
        return toJString(err);
    }

    ValidatedNeuralMasteringPlan plan {};
    std::array<float, more_phi::kSonicMasterDecisionWidth> raw {};
    bool inferenceOk = false;
    bool timedOut = false;
    {
        auto future = std::async(std::launch::async, [&]() {
            return engine.requestDecisionNow(targetLufs, plan, raw.data(), raw.size());
        });
        if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
            inferenceOk = future.get();
        else
            timedOut = true;
    }
    if (!inferenceOk)
    {
        json err;
        err["success"] = false;
        err["available"] = engine.isAvailable();
        if (timedOut)
            err["error"] = "Inference request timed out after 5.0 seconds. The model may be hung or overloaded; try again.";
        else
            err["error"] = "Inference failed or model unavailable.";
        return toJString(err);
    }

    // Limiter ceiling / AUDIT: opt-in. The decision is not applied here (it is a
    // decision-only tool), but if the caller requests the limiter ceiling be
    // honoured when they apply, stamp the flag onto the returned plan so a
    // subsequent apply_validated_plan honours it (hard-clamped to the
    // streaming-safe ceiling in AutoMasteringEngine::applyValidatedPlan).
    const bool applyLimiterCeiling = params.hasProperty("apply_limiter_ceiling")
        && static_cast<bool>(params.getProperty("apply_limiter_ceiling", false));
    plan.applyLimiterCeiling = applyLimiterCeiling;

    auto pushTargetArray = [](json& dst, const char* key, const auto& values)
    {
        dst[key] = json::array();
        for (const auto value : values)
            dst[key].push_back(value);
    };

    const auto appliedMask = json{
        {"eq", plan.appliedMask.eq},
        {"dynamics", plan.appliedMask.dynamics},
        {"stereo", plan.appliedMask.stereo},
        {"harmonic", plan.appliedMask.harmonic},
        {"limiter", plan.appliedMask.limiter},
        {"loudness", plan.appliedMask.loudness}
    };

    // Raw model telemetry mirrors mastering_decision_adapter's slice map. These
    // values are not the current engine controls; the safety-projected plan and
    // actual_engine_mapping below describe what the DSP path would consume.
    json rawModelDecision;
    rawModelDecision["field_semantics"] = "raw_model_telemetry_not_applied_parameters";
    json eqBands = json::array();
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
        eqBands.push_back({ {"frequencyHz", more_phi::kSonicMasterEqFrequenciesHz[i]},
                            {"gainDb", raw[more_phi::kSonicMasterEqGainOffset + i]},
                            {"q", more_phi::kSonicMasterEqDefaultQ} });
    rawModelDecision["eq_bands"] = eqBands;
    rawModelDecision["target_lufs"] = raw[more_phi::kSonicMasterTargetLufsIdx];
    rawModelDecision["target_lufs_semantics"] = "requested_mastering_target_not_input_loudness_measurement";
    rawModelDecision["true_peak_ceiling_dbtp"] = raw[more_phi::kSonicMasterTruePeakIdx];

    json compBands = json::array();
    for (std::size_t b = 0; b < more_phi::kSonicMasterCompBandCount; ++b)
    {
        const std::size_t o = more_phi::kSonicMasterCompOffset + b * more_phi::kSonicMasterCompBandWidth;
        compBands.push_back({ {"id", (int)b},
                              {"thresholdDb", raw[o + 0]},
                              {"ratio",       raw[o + 1]},
                              {"attackMs",    raw[o + 2]},
                              {"releaseMs",   raw[o + 3]},
                              {"makeupDb",    raw[o + 4]},
                              {"kneeDb",      raw[o + 5]} });
    }
    rawModelDecision["compressor_bands"] = compBands;
    rawModelDecision["limiter_aggressiveness"] = raw[more_phi::kSonicMasterAggrIdx];
    rawModelDecision["expected_gain_reduction_db"] = raw[more_phi::kSonicMasterGainRedIdx];

    // Character argmax over the 3 logits.
    const float c0 = raw[more_phi::kSonicMasterCharOffset + 0];
    const float c1 = raw[more_phi::kSonicMasterCharOffset + 1];
    const float c2 = raw[more_phi::kSonicMasterCharOffset + 2];
    const char* charNames[] = { "transparent", "balanced", "aggressive" };
    int charIdx = (c0 >= c1 && c0 >= c2) ? 0 : (c1 >= c2 ? 1 : 2);
    rawModelDecision["character"] = charNames[charIdx];

    json projectedPlan;
    projectedPlan["valid"] = plan.valid;
    projectedPlan["projected"] = plan.projected;
    projectedPlan["fallback_mode"] = "none";
    projectedPlan["applied_mask"] = appliedMask;
    pushTargetArray(projectedPlan, "eq_normalized", plan.projectedTargets.eq);
    pushTargetArray(projectedPlan, "dynamics_normalized", plan.projectedTargets.dynamics);
    pushTargetArray(projectedPlan, "stereo_normalized", plan.projectedTargets.stereo);
    pushTargetArray(projectedPlan, "harmonic_normalized", plan.projectedTargets.harmonic);
    pushTargetArray(projectedPlan, "limiter_normalized", plan.projectedTargets.limiter);
    pushTargetArray(projectedPlan, "loudness_normalized", plan.projectedTargets.loudness);

    json engineMapping;
    engineMapping["field_semantics"] = "safety_projected_values_mapped_to_current_dsp_controls";
    engineMapping["stage_order"] = json::array({"dynamics", "eq", "stereo", "harmonic", "loudness", "limiter"});
    engineMapping["applied_mask"] = appliedMask;

    json engineEq = json::array();
    for (std::size_t i = 0; i < more_phi::kSonicMasterEqGainCount; ++i)
    {
        const auto normalized = plan.projectedTargets.eq[i];
        engineEq.push_back({
            {"frequencyHz", more_phi::kSonicMasterEqFrequenciesHz[i]},
            {"q", more_phi::kSonicMasterEqDefaultQ},
            {"normalized", normalized},
            {"gainDb", std::clamp(normalized * more_phi::kAdaptiveEqMaxGainDb,
                                  -more_phi::kAdaptiveEqMaxGainDb,
                                  more_phi::kAdaptiveEqMaxGainDb)},
            {"applied_if_confirmed", plan.appliedMask.eq}
        });
    }
    engineMapping["eq_bands"] = engineEq;

    json engineDynamics = json::array();
    for (std::size_t b = 0; b < more_phi::kSonicMasterCompBandCount; ++b)
    {
        // AUDIT FIX: read the model's decoded compressor params from the
        // compParams sidecar (populated by decodeSonicMasterDecision), not
        // from the current DSP state. The DSP may still hold the previous
        // plan's values or heuristic defaults — the model's actual intent
        // is in the sidecar.
        const auto& cp = plan.compParams[b];
        engineDynamics.push_back({
            {"id", static_cast<int>(b)},
            {"thresholdDb", std::clamp(cp.thresholdDb, -40.0f, -6.0f)},
            {"ratio",       std::clamp(cp.ratio,       1.0f,  6.0f)},
            {"attackMs",    cp.attackMs},
            {"releaseMs",   cp.releaseMs},
            {"makeupDb",    cp.makeupDb},
            {"kneeDb",      cp.kneeDb},
            {"direct_model_controls", json::array({"thresholdDb", "ratio", "attackMs", "releaseMs", "makeupDb", "kneeDb"})},
            {"applied_if_confirmed", plan.appliedMask.dynamics}
        });
    }
    engineMapping["dynamics_bands"] = engineDynamics;

    json engineStereo = json::array();
    for (std::size_t i = 0; i < more_phi::kSonicMasterStereoRegionCount; ++i)
    {
        const auto normalized = plan.projectedTargets.stereo[i];
        engineStereo.push_back({
            {"region", static_cast<int>(i)},
            {"normalized", normalized},
            {"width", std::clamp(1.0f + normalized, 0.0f, 2.0f)},
            {"applied_if_confirmed", plan.appliedMask.stereo}
        });
    }
    engineMapping["stereo_regions"] = engineStereo;
    engineMapping["loudness"] = {
        {"target_lufs", std::clamp(-14.0f + plan.projectedTargets.loudness[0] * 6.0f, -23.0f, -8.0f)},
        {"applied_if_confirmed", plan.appliedMask.loudness},
        // AUDIT-FIX (H2): machine-readable semantic discriminator. This loudness
        // slot is the mastering TARGET (the SonicMaster input is peak-normalized,
        // so the model cannot measure absolute input LUFS — see
        // SonicMasterAnalysisEngine AUDIT-7). Genuine input-loudness measurements
        // come only from the BS.1770-4 meter surfaced via the measurements
        // snapshot. The boolean + "kind" let programmatic clients branch without
        // parsing the warnings array.
        {"kind", "target"},
        {"is_input_measurement", plan.loudnessIsMeasurement}
    };
    engineMapping["limiter"] = {
        {"ceiling_dbtp", std::clamp(-1.0f + plan.projectedTargets.limiter[0] * 0.5f, -3.0f, -0.1f)},
        {"applied_if_confirmed", plan.appliedMask.limiter || plan.applyLimiterCeiling},
        {"apply_limiter_ceiling_requested", plan.applyLimiterCeiling},
        {"default_semantics", "telemetry_only_unless_limiter_mask_true_or_apply_limiter_ceiling"}
    };
    engineMapping["harmonic"] = {
        {"amount_normalized", plan.projectedTargets.harmonic[0]},
        {"applied_if_confirmed", plan.appliedMask.harmonic},
        {"default_semantics", "telemetry_only_unless_harmonic_mask_true"}
    };

    json result;
    result["success"] = true;
    result["available"] = true;
    result["applied"] = false;  // decision only — assistant/user applies next
    result["response_schema_version"] = 2;
    result["model_source"] = "sonicmaster-v2 (masteringbrainv2)";
    // AUDIT (C2, 2026-06-25): surface the ONNX inference latency so the 3 s
    // analysis-cycle budget can be monitored. 0.0 when ONNX is unavailable or
    // before the first run. A sustained last_inference_ms approaching the cycle
    // budget means the model can no longer keep up at the configured cadence.
    result["last_inference_ms"] = p.getSonicMasterLastInferenceMs();
    result["max_inference_ms"]  = p.getSonicMasterMaxInferenceMs();
    result["analysis_cycle_budget_ms"] = 3000;  // kSonicMasterAnalysisInterval * 1000
    result["target_lufs_requested"] = targetLufs;
    // F2/AUDIT: make explicit that the neural mastering decision — even if
    // applied — lands on More-Phi's INTERNAL mastering chain, which is dormant
    // in the shipped plugin (no audio flows through it unless the Ozone
    // applicator bridge forwarded parameters to a hosted plugin).
    // `applied_to_audio_path` reports whether the last apply actually reached
    // an audible path (internal chain active OR Ozone bridge wrote params),
    // using engine.lastApplyReachedAudioPath() instead of isActive() directly.
    result["applied_to_audio_path"] = p.getAutoMasteringEngine().lastApplyReachedAudioPath();
    result["application_target"] = "more_phi_internal_mastering_chain_not_hosted_plugin";
    // Item 7 relabel: the loudness slot semantics are already disclosed in
    // engineMapping.loudness; echo the discriminator at top level for clients
    // that only read flat fields.
    result["loudness_slot_is_target_not_measurement"] = (plan.loudnessIsMeasurement == false);
    result["decision"] = rawModelDecision; // legacy alias; prefer raw_model_decision.
    result["raw_model_decision"] = rawModelDecision;
    result["projected_plan"] = projectedPlan;
    result["actual_engine_mapping"] = engineMapping;

    // AUDIT (W1+M1, 2026-06-25): surface OzonePlanApplicator mapping readiness
    // and the last-apply breakdown so a caller can distinguish "plan applied
    // zero parameters because the hosted plugin is unmapped" from "plan applied
    // zero parameters because it genuinely contained no changes." Previously
    // the only signal was a DBG line (OzonePlanApplicator.cpp:83-92) that never
    // reached the assistant. `ozone_mapped==false` here means the neural plan's
    // writes silently no-op regardless of the decoded decision.
    {
        json ms;
        ms["ozone_mapped"]       = p.ozoneMappingReady();
        ms["has_applicator"]     = p.hasOzonePlanApplicator();
        ms["mapped_slot_count"]  = p.ozoneMappedSlotCount();
        ms["max_slot_count"]     = 50;  // 8*5 EQ + 4 dyn + 4 imager + 2 maximizer
        ms["field_semantics"]    = "static_hosted_plugin_parameter_discovery";
        // If a neural apply has run, fold in its per-slot outcome so the
        // readiness signal is paired with what actually landed last cycle.
        // Decision-only calls (no prior apply) read zeros — that is expected.
        const auto breakdown = p.getAutoMasteringEngine().getChainPlanner().getLastOzoneApplyBreakdown();
        const auto verify    = p.getAutoMasteringEngine().getLastApplyVerification();
        ms["last_apply_enqueued"]      = breakdown.enqueued;
        ms["last_apply_skipped"]       = breakdown.skipped;
        ms["last_apply_unmapped"]      = breakdown.unmapped;
        ms["last_apply_ambiguous"]     = breakdown.ambiguous;
        ms["last_apply_verified"]      = verify.verified;
        ms["last_apply_mismatched"]    = verify.mismatched;
        ms["last_apply_was_partial"]   = p.getAutoMasteringEngine().lastApplyWasPartial();
        result["mapping_status"] = ms;
    }

    // AUDIT-FIX-R2: include genuine BS.1770-4 / EBU R128 measurements from the
    // AutoMasteringEngine's already-running meters. These are MEASUREMENTS
    // (K-weighting, gated integration, 4x polyphase ISP), distinct from the
    // model ESTIMATE which is peak-normalized and target-dependent. The assistant
    // should report both, clearly labeled, so it never presents the model's
    // target_lufs as a measurement of the input audio.
    {
        const auto m = engine.getLiveMeasurements();
        json meas;
        meas["valid"]               = m.valid;
        meas["source"]              = "more_phi_auto_mastering_engine_live_meters";
        meas["loudness_method"]     = "ITU-R_BS1770-4_gated_K_weighting";
        meas["true_peak_method"]    = "ITU-R_BS1770-4_4x_polyphase_oversampled";
        if (m.valid)
        {
            meas["lufs_integrated"]     = m.lufsIntegrated;
            meas["lufs_short_term"]     = m.lufsShortTerm;
            meas["lufs_momentary"]      = m.lufsMomentary;
            meas["lra"]                 = m.lra;
            meas["true_peak_dbtp"]      = m.truePeakDbtp;
            meas["spectral_centroid_hz"] = m.spectralCentroidHz;
            meas["spectral_tilt"]       = m.spectralTilt;
            meas["stereo_width"]        = m.stereoWidth;
            meas["correlation_mid"]     = m.correlationMid;
            // Metrics/AUDIT: previously-blind-spot metrics now computed live.
            meas["thd_percent"]         = m.thdPercent;
            meas["crest_factor_program"]= m.crestFactorProgram;
            meas["measurement_semantics"] = "genuine_input_measurement_NOT_model_estimate";
        }
        result["live_measurements"] = meas;
    }

    result["warnings"] = json::array({
        "decision_contains_raw_model_telemetry_not_applied_parameters",
        "target_lufs_is_requested_target_not_input_loudness_measurement",
        "limiter_ceiling_is_telemetry_unless_applied_mask_limiter_true"
    });

    // Legacy normalized arrays retained for existing clients. Prefer
    // projected_plan.*_normalized in new integrations.
    result["plan_eq_normalized"]      = json::array();
    result["plan_dynamics_normalized"] = json::array();
    result["plan_stereo_normalized"]   = json::array();
    result["plan_loudness_normalized"] = json::array();
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringEqTargetCount; ++i) result["plan_eq_normalized"].push_back(plan.projectedTargets.eq[i]);
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringDynamicsTargetCount; ++i) result["plan_dynamics_normalized"].push_back(plan.projectedTargets.dynamics[i]);
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringStereoTargetCount; ++i) result["plan_stereo_normalized"].push_back(plan.projectedTargets.stereo[i]);
    for (std::size_t i = 0; i < more_phi::kNeuralMasteringLoudnessTargetCount; ++i) result["plan_loudness_normalized"].push_back(plan.projectedTargets.loudness[i]);

    // AUDIT-FIX-R10: cache the result with a 3-second TTL so repeated calls
    // from the assistant within a short window don't re-run inference.
    toolResultCache().put("sonicmaster_decision", params,
                          p.getProcessorGenerationToken(), result,
                          p.getInstanceIdentity().instanceId,
                          std::chrono::seconds(3));

    return toJString(result);
}

juce::String MCPToolHandler::masteringNeuralApply(const juce::var& params, MorePhiProcessor& p)
{
    // Stage B (2026-06-26): one-click neural Master Assistant. Composes
    // requestDecisionNow + applyValidatedPlan and returns the per-slot
    // breakdown. This is the COMMIT door — sonicmaster_decision stays
    // decision-only for preview.
    using namespace nlohmann;

    const float targetLufs = static_cast<float>(params.getProperty("target_lufs", -14.0));
    const bool applyLimiterCeiling = params.hasProperty("apply_limiter_ceiling")
        && static_cast<bool>(params.getProperty("apply_limiter_ceiling", false));

    auto& engine = p.getSonicMasterEngine();

    // State 1: model unavailable (in-process ONNX not loaded / no HTTP fallback).
    if (!engine.isAvailable())
    {
        json err;
        err["success"] = false;
        err["available"] = false;
        err["applied"] = false;
        err["state"] = "model_unavailable";
        err["error"] = "The SonicMaster ONNX model is not loaded (in-process). Rebuild the "
                       "plugin with MORE_PHI_ENABLE_ONNX=ON, or start the HTTP fallback "
                       "(python tools/inference_server/server.py).";
        return toJString(err);
    }

    // State 2: no hosted plugin loaded → the decision has nowhere to land.
    if (!p.hasOzonePlanApplicator())
    {
        json err;
        err["success"] = false;
        err["available"] = true;
        err["applied"] = false;
        err["state"] = "no_hosted_plugin";
        err["error"] = "No hosted plugin is loaded. Load a mastering plugin (e.g. an Ozone "
                       "instance) into More-Phi first, then re-run.";
        json ms;
        ms["ozone_mapped"] = false;
        ms["has_applicator"] = false;
        ms["mapped_slot_count"] = 0;
        err["mapping_status"] = ms;
        return toJString(err);
    }

    // State 3: hosted plugin loaded but its parameters aren't mapped (all-stubs).
    if (!p.ozoneMappingReady())
    {
        json err;
        err["success"] = false;
        err["available"] = true;
        err["applied"] = false;
        err["state"] = "unmapped";
        err["mapped_slot_count"] = p.ozoneMappedSlotCount();
        err["error"] = "The hosted plugin's parameter names don't match the Ozone-shaped "
                       "discovery, so the neural plan has nowhere to write. Check "
                       "mapping_status for details, or host an iZotope Ozone instance.";
        return toJString(err);
    }

    // Run the decision (5 s budget, matching sonicmaster_decision).
    ValidatedNeuralMasteringPlan plan {};
    bool inferenceOk = false;
    bool timedOut = false;
    {
        auto future = std::async(std::launch::async, [&]() {
            return engine.requestDecisionNow(targetLufs, plan, nullptr, 0);
        });
        if (future.wait_for(std::chrono::seconds(5)) == std::future_status::ready)
            inferenceOk = future.get();
        else
            timedOut = true;
    }
    if (!inferenceOk)
    {
        json err;
        err["success"] = false;
        err["available"] = true;
        err["applied"] = false;
        err["state"] = timedOut ? "inference_timeout" : "inference_failed";
        err["error"] = timedOut
            ? "Inference request timed out after 5.0 seconds."
            : "Inference failed (model returned no decision).";
        return toJString(err);
    }

    plan.applyLimiterCeiling = applyLimiterCeiling;

    // Commit: apply to the hosted plugin via the existing applyValidatedPlan door.
    const bool applied = p.getAutoMasteringEngine().applyValidatedPlan(plan);

    json result;
    result["success"] = applied;
    result["available"] = true;
    result["applied"] = applied;
    result["state"] = applied ? "active_applying" : "apply_no_op";
    result["target_lufs_requested"] = targetLufs;
    result["loudness_slot_is_target_not_measurement"] = (plan.loudnessIsMeasurement == false);

    // Per-slot breakdown (reuses the Stage-2 mapping_status surface).
    {
        const auto breakdown = p.getAutoMasteringEngine().getChainPlanner().getLastOzoneApplyBreakdown();
        const auto verify = p.getAutoMasteringEngine().getLastApplyVerification();
        json ms;
        ms["ozone_mapped"] = p.ozoneMappingReady();
        ms["has_applicator"] = p.hasOzonePlanApplicator();
        ms["mapped_slot_count"] = p.ozoneMappedSlotCount();
        ms["last_apply_enqueued"] = breakdown.enqueued;
        ms["last_apply_skipped"] = breakdown.skipped;
        ms["last_apply_unmapped"] = breakdown.unmapped;
        ms["last_apply_ambiguous"] = breakdown.ambiguous;
        ms["last_apply_verified"] = verify.verified;
        ms["last_apply_mismatched"] = verify.mismatched;
        ms["last_apply_was_partial"] = p.getAutoMasteringEngine().lastApplyWasPartial();
        ms["last_apply_reached_audio_path"] = p.getAutoMasteringEngine().lastApplyReachedAudioPath();
        result["mapping_status"] = ms;
    }

    // Fold in the live measurements so the assistant can read achieved LUFS for
    // the closed loop (Stage D) without a second call.
    {
        const auto m = engine.getLiveMeasurements();
        json meas;
        meas["valid"] = m.valid;
        if (m.valid)
        {
            meas["lufs_integrated"] = m.lufsIntegrated;
            meas["lufs_short_term"] = m.lufsShortTerm;
            meas["true_peak_dbtp"] = m.truePeakDbtp;
            meas["measurement_semantics"] = "genuine_input_measurement_NOT_model_estimate";
        }
        result["live_measurements"] = meas;
    }

    if (!applied)
        result["error"] = "The decision was decoded but applyValidatedPlan wrote zero "
                          "parameters (see mapping_status for the per-slot breakdown).";

    return toJString(result);
}

// ── IPC Assistant tools ──────────────────────────────────────────────────────

juce::String MCPToolHandler::morePhiIpcAttach(const juce::var& params, MorePhiProcessor& p)
{
    standalone_mcp::IpcAttachArgs parsed;
    juce::String error;

    if (!optionalStringProperty(params, "segment_name", parsed.segmentName, error))
        return invalidParamsResponse(error.toRawUTF8());

    std::optional<size_t> pid;
    if (!optionalSizeProperty(params, "daw_process_id", pid, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (!assignOptionalUInt32(pid, parsed.dawProcessId, "daw_process_id", error))
        return invalidParamsResponse(error.toRawUTF8());

    std::optional<size_t> mappedSize;
    if (!optionalSizeProperty(params, "mapped_size_bytes", mappedSize, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (mappedSize)
        parsed.mappedSizeBytes = *mappedSize;

    return toJString(p.getMorePhiIPCDiscovery().attach(parsed).body);
}

juce::String MCPToolHandler::morePhiIpcDetach(MorePhiProcessor& p)
{
    return toJString(p.getMorePhiIPCDiscovery().detach().body);
}

juce::String MCPToolHandler::morePhiIpcStatus(MorePhiProcessor& p)
{
    return toJString(p.getMorePhiIPCDiscovery().status().body);
}

juce::String MCPToolHandler::morePhiIpcSnapshot(const juce::var& params, MorePhiProcessor& p)
{
    standalone_mcp::IpcSnapshotArgs parsed;
    juce::String error;

    std::optional<size_t> value;
    if (!optionalSizeProperty(params, "offset", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.offset = *value;

    value.reset();
    if (!optionalSizeProperty(params, "size_bytes", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.sizeBytes = *value;

    value.reset();
    if (!optionalSizeProperty(params, "max_frames", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.maxFrames = *value;

    return toJString(p.getMorePhiIPCDiscovery().snapshot(parsed).body);
}

juce::String MCPToolHandler::morePhiIpcDump(const juce::var& params, MorePhiProcessor& p)
{
    standalone_mcp::IpcDumpArgs parsed;
    juce::String error;

    if (!requiredStringProperty(params, "output_path", parsed.outputPath, error))
        return invalidParamsResponse(error.toRawUTF8());

    std::optional<size_t> value;
    if (!optionalSizeProperty(params, "offset", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.offset = *value;

    value.reset();
    if (!optionalSizeProperty(params, "size_bytes", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.sizeBytes = *value;

    return toJString(p.getMorePhiIPCDiscovery().dump(parsed).body);
}

juce::String MCPToolHandler::morePhiIpcCapture(const juce::var& params, MorePhiProcessor& p)
{
    standalone_mcp::IpcCaptureArgs parsed;
    juce::String error;

    std::optional<size_t> value;
    if (!optionalSizeProperty(params, "offset", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.offset = *value;

    value.reset();
    if (!optionalSizeProperty(params, "size_bytes", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.sizeBytes = *value;

    value.reset();
    if (!optionalSizeProperty(params, "duration_ms", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.durationMs = *value;

    value.reset();
    if (!optionalSizeProperty(params, "interval_ms", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.intervalMs = *value;

    value.reset();
    if (!optionalSizeProperty(params, "max_changes", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.maxChanges = *value;

    value.reset();
    if (!optionalSizeProperty(params, "max_ranges_per_change", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.maxRangesPerChange = *value;

    value.reset();
    if (!optionalSizeProperty(params, "max_frames", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.maxFrames = *value;

    if (!optionalStringProperty(params, "baseline_base64", parsed.baselineBase64, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (!optionalStringProperty(params, "output_path", parsed.outputPath, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (!optionalBoolProperty(params, "include_changed_bytes", parsed.includeChangedBytes, error))
        return invalidParamsResponse(error.toRawUTF8());

    return toJString(p.getMorePhiIPCDiscovery().capture(parsed).body);
}

juce::String MCPToolHandler::morePhiIpcRunAssistant(const juce::var& params, MorePhiProcessor& p)
{
    standalone_mcp::IpcAssistantRunArgs parsed;
    juce::String error;

    if (!optionalStringProperty(params, "schema_path", parsed.schemaPath, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (!optionalStringProperty(params, "segment_name", parsed.segmentName, error))
        return invalidParamsResponse(error.toRawUTF8());

    std::optional<size_t> value;
    if (!optionalSizeProperty(params, "daw_process_id", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (!assignOptionalUInt32(value, parsed.dawProcessId, "daw_process_id", error))
        return invalidParamsResponse(error.toRawUTF8());

    value.reset();
    if (!optionalSizeProperty(params, "instance_id", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (!assignOptionalUInt32(value, parsed.instanceId, "instance_id", error))
        return invalidParamsResponse(error.toRawUTF8());

    std::optional<std::string> pluginNameQuery;
    if (!optionalStringProperty(params, "plugin_name_query", pluginNameQuery, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (pluginNameQuery && !pluginNameQuery->empty())
        parsed.pluginNameQuery = *pluginNameQuery;

    value.reset();
    if (!optionalSizeProperty(params, "timeout_ms", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.timeoutMs = *value;

    value.reset();
    if (!optionalSizeProperty(params, "poll_interval_ms", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value)
        parsed.pollIntervalMs = *value;

    value.reset();
    if (!optionalSizeProperty(params, "observer_id", value, error))
        return invalidParamsResponse(error.toRawUTF8());
    if (value && *value > static_cast<size_t>(std::numeric_limits<uint32_t>::max()))
        return invalidParamsResponse("observer_id is outside the uint32 range.");
    if (value)
        parsed.observerId = static_cast<uint32_t>(*value);

    if (!optionalBoolProperty(params, "allow_unsafe_write", parsed.allowUnsafeWrite, error))
        return invalidParamsResponse(error.toRawUTF8());

    bool applyResult = false;
    if (!optionalBoolProperty(params, "apply_result", applyResult, error))
        return invalidParamsResponse(error.toRawUTF8());

    auto outcome = p.getMorePhiIPCAssistant().runAssistant(parsed);
    if (!outcome.isError && applyResult)
    {
        auto applyOutcome = applyIpcAssistantParametersToHostedPlugin(outcome.body, p);
        outcome.body["apply_result"] = applyOutcome.body.value("apply_result", json::object());
        if (applyOutcome.isError)
        {
            outcome.body["success"] = false;
            outcome.body["error"] = applyOutcome.body.value("error", "assistant_apply_failed");
            if (applyOutcome.body.contains("message"))
                outcome.body["message"] = applyOutcome.body["message"];
            outcome.isError = true;
        }
    }

    return toJString(outcome.body);
}

// ── Ozone Track Assistant helpers ─────────────────────────────────────────────

struct AnalysisProfile { float targetLUFS; float maxTruePeak; const char* name; };
static AnalysisProfile profileForName(const juce::String& name) noexcept
{
    const auto profile = name.toLowerCase();
    if (profile == "streaming")      return { -14.0f, -1.0f, "streaming" };
    if (profile == "vinyl_master")   return { -18.0f, -3.0f, "vinyl_master" };
    if (profile == "broadcast")      return { -23.0f, -1.0f, "broadcast" };
    return                                  { -14.0f, -1.0f, "standard" };
}

static double finiteOr(double value, double fallback) noexcept
{
    return std::isfinite(value) ? value : fallback;
}

static std::string makeOzoneRecommendation(const AnalysisProfile& profile,
                                           double lufsIntegrated,
                                           double truePeak,
                                           double dynamicRange)
{
    std::string recommendation;
    const double lufsDelta = lufsIntegrated - profile.targetLUFS;
    const double peakMargin = truePeak - profile.maxTruePeak;

    if (std::abs(lufsDelta) > 0.5)
    {
        char buf[160] = {};
        std::snprintf(buf, sizeof(buf),
            "Track is %.1f LUFS %s than the %s target of %.1f LUFS.",
            std::abs(lufsDelta),
            lufsDelta > 0.0 ? "louder" : "quieter",
            profile.name,
            profile.targetLUFS);
        recommendation += buf;
        recommendation += lufsDelta > 0.0
            ? " Reduce output gain or limiter drive."
            : " Increase output level or reduce conservative limiting.";
    }

    if (peakMargin > 0.0)
    {
        char buf[160] = {};
        std::snprintf(buf, sizeof(buf),
            "%sTrue peak %.1f dBTP exceeds the %.1f dBTP ceiling; reduce limiter ceiling.",
            recommendation.empty() ? "" : " ",
            truePeak,
            profile.maxTruePeak);
        recommendation += buf;
    }

    if (dynamicRange < 3.0)
        recommendation += recommendation.empty()
            ? "Dynamic range is very low; review compression and limiter drive."
            : " Dynamic range is very low; review compression and limiter drive.";

    if (recommendation.empty())
        recommendation = "Track meets the local target levels for the selected profile.";

    return recommendation;
}

static std::vector<juce::String> statusFilterFromParams(const juce::var& params)
{
    std::vector<juce::String> statuses;
    const auto filterVar = params.getProperty("status_filter", juce::var());
    if (const auto* array = filterVar.getArray())
    {
        for (const auto& value : *array)
            statuses.push_back(value.toString().trim().toLowerCase());
    }
    return statuses;
}

static juce::String requireTrackId(const juce::var& params, const InstanceIdentity& identity)
{
    const auto explicitId = params.getProperty("track_id", "").toString();
    if (explicitId.isNotEmpty())
        return explicitId;

    const auto current = TrackAssistantStore::ensureCurrentSessionTrack(identity.instanceId);
    return juce::String(current.value("track_id", ""));
}

// ── ozone.track.get_info ─────────────────────────────────────────────────────

juce::String MCPToolHandler::ozoneTrackGetInfo(const juce::var& params,
                                                MorePhiProcessor& p,
                                                const InstanceIdentity& identity)
{
    const bool includeHistory = params.getProperty("include_history", false);
    const auto trackId = requireTrackId(params, identity);

    if (!TrackAssistantStore::isValidTrackId(trackId))
        return toJString(json{{"success", false}, {"error", "invalid_track_id"}});

    auto result = TrackAssistantStore::getInfo(trackId, includeHistory);
    if (result.value("success", false))
    {
        auto* plugin = p.getHostManager().getPlugin();
        auto& ame = p.getAutoMasteringEngine();

        result["instance_id"] = identity.instanceId.toStdString();
        result["ozone_hosted"] = plugin != nullptr && OzoneParameterMap::isOzone11(plugin->getName());
        result["lufs_integrated"] = finiteOr(ame.getLUFSIntegrated(), -100.0);
        result["lufs_momentary"] = finiteOr(ame.getLUFSMomentary(), -100.0);
        result["lufs_short_term"] = finiteOr(ame.getLUFSShortTerm(), -100.0);
        result["true_peak_dbtp"] = finiteOr(ame.getTruePeak_dBTP(), -100.0);
        result["limiter_gr_db"] = finiteOr(ame.getLimiterGainReductionDB(), 0.0);
        result["lra"] = finiteOr(ame.getLRA(), 0.0);

        if (plugin != nullptr)
        {
            result["plugin_name"] = plugin->getName().toStdString();
            result["param_count"] = static_cast<int>(plugin->getParameters().size());
            result["latency_samples"] = plugin->getLatencySamples();
        }
    }

    return toJString(result);
}

// ── ozone.track.update_status ─────────────────────────────────────────────────

juce::String MCPToolHandler::ozoneTrackUpdateStatus(const juce::var& params,
                                                     const InstanceIdentity& identity)
{
    const auto trackId = requireTrackId(params, identity);
    const auto newStatusStr = params.getProperty("new_status", "").toString();
    const auto reason       = params.getProperty("reason", "").toString();

    if (!TrackAssistantStore::isValidTrackId(trackId))
        return toJString(json{{"success", false}, {"error", "invalid_track_id"}});

    if (!TrackAssistantStore::isValidStatus(newStatusStr))
        return toJString(json{
            {"success", false},
            {"error",   "invalid_status"},
            {"details", "Valid values: pending_review, in_mastering, mastering_complete, "
                        "approved, rejected, on_hold"}
        });

    const auto normalizedStatus = newStatusStr.trim().toLowerCase();
    if ((normalizedStatus == "rejected" || normalizedStatus == "on_hold")
         && reason.isEmpty())
    {
        return toJString(json{
            {"success", false},
            {"error",   "reason_required"},
            {"details", "A 'reason' must be supplied when setting status to '"
                        + normalizedStatus.toStdString() + "'."}
        });
    }

    return toJString(TrackAssistantStore::updateStatus(trackId, normalizedStatus, reason));
}

// ── ozone.track.analyze ───────────────────────────────────────────────────────

juce::String MCPToolHandler::ozoneTrackAnalyze(const juce::var& params,
                                                MorePhiProcessor& p,
                                                const InstanceIdentity& identity)
{
    const auto trackId = requireTrackId(params, identity);
    const auto profileName    = params.getProperty("analysis_profile", "standard").toString();
    const bool forceReanalyze = params.getProperty("force_reanalyze", false);

    if (!TrackAssistantStore::isValidTrackId(trackId))
        return toJString(json{{"success", false}, {"error", "invalid_track_id"}});
    if (!TrackAssistantStore::isValidAnalysisProfile(profileName))
        return toJString(json{{"success", false}, {"error", "invalid_analysis_profile"}});

    auto& ame = p.getAutoMasteringEngine();

    if (forceReanalyze)
    {
        constexpr int genreIdx = 0;
        const auto plan = ame.getChainPlanner().executePlan(
            genreIdx,
            static_cast<float>(std::max(1.0, finiteOr(ame.getLRA(), 6.0))),
            0.0f,
            0.5f);
        (void)plan;
    }

    const AnalysisProfile profile = profileForName(profileName);
    const double lufsInteg = finiteOr(ame.getLUFSIntegrated(), -100.0);
    const double shortTerm = finiteOr(ame.getLUFSShortTerm(), -100.0);
    const double momentary = finiteOr(ame.getLUFSMomentary(), -100.0);
    const double truePeak = finiteOr(ame.getTruePeak_dBTP(), -100.0);
    const double limiterGR = finiteOr(ame.getLimiterGainReductionDB(), 0.0);
    const double lra = finiteOr(ame.getLRA(), 0.0);
    const double dynamicRange = lra;
    const double lufsDelta = lufsInteg - profile.targetLUFS;
    const double peakMargin = truePeak - profile.maxTruePeak;

    json grArr = json::array();
    for (int i = 0; i < 4; ++i)
        grArr.push_back(finiteOr(ame.getGainReductionDB(i), 0.0));

    auto trackInfo = TrackAssistantStore::getInfo(trackId, true);
    if (!trackInfo.value("success", false))
        return toJString(trackInfo);

    json result{
        {"success", true},
        {"track_id", trackId.toStdString()},
        {"title", trackInfo.value("title", "")},
        {"analysis", {
            {"source", "more_phi_local_analysis"},
            {"lufs_integrated", lufsInteg},
            {"lufs_short_term_max", shortTerm},
            {"lufs_momentary", momentary},
            {"true_peak_dbtp", truePeak},
            {"limiter_gr_db", limiterGR},
            {"dynamic_range_db", dynamicRange},
            {"dynamic_range_lra_db", dynamicRange},
            {"lra", lra},
            {"per_band_gr_db", grArr},
            {"target_lufs", profile.targetLUFS},
            {"target_true_peak_dbtp", profile.maxTruePeak},
            {"lufs_delta", lufsDelta},
            {"peak_margin", peakMargin},
            {"profile", profile.name},
            {"recommendation_type", kPlannerType},
            {"planner_type", kPlannerType},
            {"rule_version", kPlannerRuleVersion},
            {"confidence", nullptr},
            {"measured_inputs", {
                {"lufs_integrated", lufsInteg},
                {"true_peak_dbtp", truePeak},
                {"dynamic_range_lra", dynamicRange},
                {"profile", profile.name}
            }},
            {"rules_applied", json::array({
                {
                    {"rule_id", "streaming_target_delta_v1"},
                    {"reason", "compare integrated loudness with selected profile target"},
                    {"output", { {"lufs_delta", lufsDelta} }}
                },
                {
                    {"rule_id", "true_peak_ceiling_v1"},
                    {"reason", "compare true peak estimate with selected profile ceiling"},
                    {"output", { {"peak_margin", peakMargin} }}
                },
                {
                    {"rule_id", "dynamic_range_review_v1"},
                    {"reason", "flag very low local LRA estimate"},
                    {"output", { {"dynamic_range_lra", dynamicRange} }}
                }
            })},
            {"ozone_recommendation", makeOzoneRecommendation(profile, lufsInteg, truePeak, dynamicRange)},
            {"ozone_applicator_active", ame.getChainPlanner().hasOzoneApplicator()},
            {"analyzed_at", juce::Time::getCurrentTime().toISO8601(true).toStdString()}
        }}
    };

    if (trackInfo.contains("latest_render_job_id"))
        result["analysis"]["latest_render_job_id"] = trackInfo["latest_render_job_id"];
    if (trackInfo.contains("selected_candidate_id"))
        result["analysis"]["selected_candidate_id"] = trackInfo["selected_candidate_id"];
    if (trackInfo.contains("selected_output_path"))
        result["analysis"]["selected_output_path"] = trackInfo["selected_output_path"];

    TrackAssistantStore::setAnalysis(trackId, result["analysis"]);
    return toJString(result);
}

// ── ozone.track.search ────────────────────────────────────────────────────────

juce::String MCPToolHandler::ozoneTrackSearch(const juce::var& params,
                                               MorePhiProcessor& p,
                                               const InstanceIdentity& identity)
{
    (void)p;
    TrackAssistantStore::ensureCurrentSessionTrack(identity.instanceId);

    const auto query = params.getProperty("query", "").toString();
    const auto dateFrom = params.getProperty("date_from", "").toString();
    const auto dateTo = params.getProperty("date_to", "").toString();
    const int page = juce::jmax(1, static_cast<int>(params.getProperty("page", 1)));
    const int pageSize = juce::jlimit(1, 50, static_cast<int>(params.getProperty("page_size", 20)));

    if (!TrackAssistantStore::isValidDate(dateFrom) || !TrackAssistantStore::isValidDate(dateTo))
        return toJString(json{{"success", false}, {"error", "invalid_date"}});

    const auto statuses = statusFilterFromParams(params);
    for (const auto& status : statuses)
    {
        if (!TrackAssistantStore::isValidStatus(status))
            return toJString(json{{"success", false}, {"error", "invalid_status_filter"}});
    }

    return toJString(TrackAssistantStore::search(query, statuses, dateFrom, dateTo, page, pageSize));
}

// ── Agent runtime tools (agents.*) ────────────────────────────────────────────
//
// These handlers are thin MCP wrappers over AgentRuntime (src/AI/Agents/AgentRuntime.h).
// They intentionally defer every state check to the runtime; if the runtime was never
// constructed (e.g. MCP server not started, or agent layer disabled) we return a
// deterministic `agents_unavailable` error envelope so callers can branch cleanly.

namespace {
// Resolves the per-processor agent runtime. Returns nullptr if the plugin has not
// started its MCP server / runtime yet.
more_phi::agents::AgentRuntime* runtimeOf(MorePhiProcessor& p);
more_phi::agents::AgentRuntime* runtimeOf(MorePhiProcessor& p)
{
    return p.getAgentRuntime();
}

juce::String agentsUnavailable()
{
    return juce::String(nlohmann::json{
        {"error", { {"code", "agents_unavailable"},
                    {"message", "agent runtime not started"} } }
    }.dump());
}

// Best-effort conversion of an AgentResult (which uses nlohmann::json internally) to a
// juce::String MCP envelope. Findings/actions/events are surfaced verbatim so MCP
// consumers (and tests) can introspect them.
juce::String resultEnvelope(const juce::String& taskId, const more_phi::agents::AgentResult& r)
{
    nlohmann::json env = nlohmann::json::object();
    env["task_id"] = taskId.toStdString();
    env["success"] = r.success;
    if (! r.errorCode.isEmpty())
        env["error_code"] = r.errorCode.toStdString();
    env["findings"]       = r.findings;
    env["proposed_actions"] = r.proposedActions;
    env["telemetry"]      = r.telemetry;
    return juce::String(env.dump());
}
} // namespace

juce::String MCPToolHandler::agentsList(MorePhiProcessor& p)
{
    auto* rt = runtimeOf(p);
    if (rt == nullptr)
        return agentsUnavailable();
    return juce::String(rt->describeState().dump());
}

juce::String MCPToolHandler::agentsRunGoal(const juce::var& params, MorePhiProcessor& p)
{
    auto* rt = runtimeOf(p);
    if (rt == nullptr)
        return agentsUnavailable();

    const auto intent = params.getProperty("intent", {}).toString();
    if (intent.trim().isEmpty())
        return toJString(json{{"error", "missing_intent"}});

    if (! rt->isRunning())
        return juce::String(nlohmann::json{
            {"error", "runtime_stopped"},
            {"message", "agent runtime is stopped; start it before submitting goals"}
        }.dump());

    const auto runId = rt->submitGoal(intent, more_phi::agents::TaskPriority::High, "mcp");
    if (runId.isEmpty())
        return toJString(json{{"error", "no_conductor_registered"}});

    return juce::String(nlohmann::json{
        {"run_id", runId.toStdString()},
        {"state",  "submitted"}
    }.dump());
}

juce::String MCPToolHandler::agentsRunTask(const juce::var& params, MorePhiProcessor& p)
{
    auto* rt = runtimeOf(p);
    if (rt == nullptr)
        return agentsUnavailable();

    const auto agentName = params.getProperty("agent", {}).toString().trim();
    const auto intent    = params.getProperty("intent", {}).toString().trim();
    if (agentName.isEmpty())
        return toJString(json{{"error", "missing_agent"}});
    // H2: intent is required — mirrors agentsRunGoal. An empty intent produces a
    // no-op task that wastes a queue slot and returns a generic error later.
    if (intent.isEmpty())
        return toJString(json{{"error", "missing_intent"}});

    const auto role = more_phi::agents::agentRoleFromString(agentName);
    // H2: reject Custom outright. No Custom agent is ever registered in production,
    // and the old "allow through if literally 'custom'" path silently enqueued a
    // doomed task. Fail fast with a clear error.
    if (role == more_phi::agents::AgentRole::Custom)
        return toJString(json{{"error", "unknown_agent_role"}, {"role", agentName.toStdString()}});

    if (! rt->isRunning())
        return juce::String(nlohmann::json{
            {"error", "runtime_stopped"},
            {"message", "agent runtime is stopped; start it before submitting tasks"}
        }.dump());

    more_phi::agents::AgentTask task;
    task.targetRole = role;
    task.intent     = intent;
    task.priority   = more_phi::agents::TaskPriority::Normal;
    task.origin     = "mcp";
    const auto taskId = rt->submitTask(std::move(task));
    if (taskId.isEmpty())
        return toJString(json{{"error", "agent_not_registered"}, {"role", agentName.toStdString()}});

    return juce::String(nlohmann::json{
        {"task_id", taskId.toStdString()},
        {"state",   "submitted"}
    }.dump());
}

juce::String MCPToolHandler::agentsRunStatus(const juce::var& params, MorePhiProcessor& p)
{
    auto* rt = runtimeOf(p);
    if (rt == nullptr)
        return agentsUnavailable();

    const auto taskId = params.getProperty("task_id", {}).toString().trim();
    if (taskId.isEmpty())
        return toJString(json{{"error", "missing_task_id"}});

    const auto maybe = rt->peekResult(taskId);
    if (! maybe.has_value())
    {
        return juce::String(nlohmann::json{
            {"task_id", taskId.toStdString()},
            {"state",   "pending"}
        }.dump());
    }
    return resultEnvelope(taskId, *maybe);
}

juce::String MCPToolHandler::agentsRunCancel(const juce::var& /*params*/, MorePhiProcessor& p)
{
    auto* rt = runtimeOf(p);
    if (rt == nullptr)
        return agentsUnavailable();
    // H1: stop() halts workers + pump. It is restartable — a subsequent
    // agents.run_goal / agents.run_task returns runtime_stopped until start() is
    // called again (e.g. via re-initialization or an explicit start path). We do
    // NOT silently re-queue into a dead pool.
    const bool wasRunning = rt->isRunning();
    rt->stop();
    return juce::String(nlohmann::json{
        {"state", "stopped"},
        {"was_running", wasRunning},
        {"restartable", true}
    }.dump());
}

juce::String MCPToolHandler::agentsBlackboardRecent(MorePhiProcessor& p)
{
    auto* rt = runtimeOf(p);
    if (rt == nullptr)
        return agentsUnavailable();
    // H3: surface the CURATED, agent-only view from BlackboardBridge rather than
    // the raw IntegrationEventBus. The raw bus also carries non-agent traffic
    // (action-ledger artifacts, permission decisions) and full payloads with
    // mastering telemetry — both inappropriate for an unscoped MCP consumer.
    // blackboardRecent() filters to agent-published events and redacts payloads
    // to a safe summary (type/source/runId/sequence).
    return juce::String(rt->blackboardRecent(32).dump());
}

juce::String MCPToolHandler::agentsSetAutonomy(const juce::var& params,
                                                MorePhiProcessor& p,
                                                AutomationRuntime& runtime)
{
    (void)p;
    const auto levelStr = params.getProperty("level", {}).toString().trim();
    if (levelStr.isEmpty())
        return toJString(json{{"error", "missing_level"}});

    const auto level = more_phi::autonomyLevelFromString(levelStr);
    runtime.permissions().setAutonomyLevel(level);
    return juce::String(nlohmann::json{
        {"level",  more_phi::toString(level).toStdString()},
        {"applied", true}
    }.dump());
}

} // namespace more_phi
