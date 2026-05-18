#include "StandaloneMcpServer.h"

#include "JsonRpc.h"

#include <algorithm>
#include <iostream>
#include <sstream>

namespace more_phi::standalone_mcp {

using json = nlohmann::json;

namespace {

json stringProperty(std::string description)
{
    return {{"type", "string"}, {"description", std::move(description)}};
}

json integerProperty(std::string description, int minimum, int defaultValue)
{
    return {
        {"type", "integer"},
        {"description", std::move(description)},
        {"minimum", minimum},
        {"default", defaultValue}
    };
}

json numberProperty(std::string description, double minimum, double maximum, double defaultValue)
{
    return {
        {"type", "number"},
        {"description", std::move(description)},
        {"minimum", minimum},
        {"maximum", maximum},
        {"default", defaultValue}
    };
}

bool hasRequiredString(const json& args, const char* key)
{
    return args.contains(key) && args[key].is_string() && !args[key].get<std::string>().empty();
}

std::optional<std::string> optionalString(const json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null() || !args[key].is_string())
        return std::nullopt;
    return args[key].get<std::string>();
}

std::optional<int> optionalInt(const json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null() || !args[key].is_number_integer())
        return std::nullopt;
    return args[key].get<int>();
}

std::optional<size_t> optionalSize(const json& args, const char* key)
{
    if (!args.contains(key) || args[key].is_null())
        return std::nullopt;

    if (args[key].is_number_unsigned())
        return args[key].get<size_t>();

    if (args[key].is_number_integer())
    {
        const auto value = args[key].get<long long>();
        if (value >= 0)
            return static_cast<size_t>(value);
    }

    return std::nullopt;
}

bool optionalBool(const json& args, const char* key, bool fallback)
{
    if (!args.contains(key) || !args[key].is_boolean())
        return fallback;
    return args[key].get<bool>();
}

double optionalNumber(const json& args, const char* key, double fallback)
{
    if (!args.contains(key) || !args[key].is_number())
        return fallback;
    return args[key].get<double>();
}

ToolCallOutcome assistantResultInvalid(std::string message)
{
    return {json{
        {"success", false},
        {"error", "assistant_result_invalid"},
        {"message", std::move(message)},
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

ToolCallOutcome parseAssistantApplyArgs(const json& body, AssistantParameterApplyArgs& out)
{
    if (!body.contains("parameters") || !body["parameters"].is_array())
        return assistantResultInvalid("parameters array is missing.");

    for (const auto& item : body["parameters"])
    {
        if (!item.is_object() || !item.contains("index") || !item["index"].is_number_integer()
            || !item.contains("value") || !item["value"].is_number())
        {
            return assistantResultInvalid("parameters entries must contain integer index and numeric value.");
        }

        out.parameters.push_back({
            item["index"].get<int>(),
            item["value"].get<double>()
        });
    }

    return {json{{"success", true}}, false};
}

} // namespace

StandaloneMcpServer::StandaloneMcpServer(std::unique_ptr<OzonePluginBackend> backendIn,
                                         std::unique_ptr<IZotopeIPCDiscovery> ipcDiscoveryIn,
                                         std::unique_ptr<IZotopeIPCAssistant> ipcAssistantIn)
    : backend(std::move(backendIn)),
      ipcDiscovery(std::move(ipcDiscoveryIn)),
      ipcAssistant(std::move(ipcAssistantIn))
{
    tools = {
        {
            "ozone_get_parameters",
            "Get Ozone Parameters",
            "Read Ozone's hosted plugin parameters and current normalized values.",
            {
                {"type", "object"},
                {"properties", {
                    {"query", stringProperty("Optional case-insensitive filter matched against parameter name or stable ID.")},
                    {"include_values", {
                        {"type", "boolean"},
                        {"default", true},
                        {"description", "Include current normalized values and display text."}
                    }}
                }}
            },
            {{"title", "Get Ozone Parameters"}, {"readOnlyHint", true}, {"openWorldHint", false}}
        },
        {
            "ozone_set_parameter",
            "Set Ozone Parameter",
            "Set one Ozone hosted-plugin parameter by normalized index.",
            {
                {"type", "object"},
                {"properties", {
                    {"index", integerProperty("Parameter index from ozone_get_parameters.", 0, 0)},
                    {"value", numberProperty("Normalized parameter value.", 0.0, 1.0, 0.0)}
                }},
                {"required", {"index", "value"}}
            },
            {{"title", "Set Ozone Parameter"}, {"destructiveHint", true}, {"idempotentHint", true}}
        },
        {
            "ozone_run_master_assistant",
            "Run Master Assistant",
            "Trigger Ozone's automatable assistant/analyze parameter when exposed by this Ozone version.",
            {
                {"type", "object"},
                {"properties", {
                    {"assistant_parameter_index", integerProperty("Optional explicit assistant/analyze parameter index.", 0, 0)},
                    {"input_audio_path", stringProperty("Optional local audio file to render through Ozone during analysis.")},
                    {"analysis_seconds", numberProperty("Maximum seconds of input_audio_path to process.", 0.01, 600.0, 30.0)}
                }}
            },
            {{"title", "Run Master Assistant"}, {"destructiveHint", true}, {"idempotentHint", false}}
        },
        {
            "ozone_get_state",
            "Get Ozone State",
            "Capture Ozone's opaque plugin state as standard Base64.",
            {
                {"type", "object"},
                {"properties", json::object()}
            },
            {{"title", "Get Ozone State"}, {"readOnlyHint", true}, {"openWorldHint", false}}
        },
        {
            "ozone_set_state",
            "Set Ozone State",
            "Restore Ozone's opaque plugin state from standard Base64.",
            {
                {"type", "object"},
                {"properties", {
                    {"state_base64", stringProperty("Base64 data previously returned by ozone_get_state.")}
                }},
                {"required", {"state_base64"}}
            },
            {{"title", "Set Ozone State"}, {"destructiveHint", true}, {"idempotentHint", true}}
        },
        {
            "izotope_ipc_attach",
            "Attach iZotope IPC",
            "Attach read-only to a named iZotope inter-plugin communication shared-memory segment.",
            {
                {"type", "object"},
                {"properties", {
                    {"segment_name", stringProperty("Explicit shared-memory segment name. Falls back to IZOTOPE_IPC_SEGMENT_NAME.")},
                    {"daw_process_id", integerProperty("Optional DAW process ID used to build a candidate segment name.", 1, 0)},
                    {"mapped_size_bytes", integerProperty("Maximum bytes to map/read from the segment.", 1, 4194304)}
                }}
            },
            {{"title", "Attach iZotope IPC"}, {"readOnlyHint", true}, {"openWorldHint", true}}
        },
        {
            "izotope_ipc_detach",
            "Detach iZotope IPC",
            "Detach from the currently mapped iZotope IPC shared-memory segment.",
            {
                {"type", "object"},
                {"properties", json::object()}
            },
            {{"title", "Detach iZotope IPC"}, {"readOnlyHint", true}, {"openWorldHint", false}}
        },
        {
            "izotope_ipc_status",
            "iZotope IPC Status",
            "Report read-only IPC attachment state and last attach error.",
            {
                {"type", "object"},
                {"properties", json::object()}
            },
            {{"title", "iZotope IPC Status"}, {"readOnlyHint", true}, {"openWorldHint", false}}
        },
        {
            "izotope_ipc_snapshot",
            "Snapshot iZotope IPC",
            "Read a bounded byte range from the attached IPC segment and return raw bytes plus heuristic frame candidates.",
            {
                {"type", "object"},
                {"properties", {
                    {"offset", integerProperty("Byte offset within the mapped segment.", 0, 0)},
                    {"size_bytes", integerProperty("Number of bytes to read, capped by the backend.", 1, 1024)},
                    {"max_frames", integerProperty("Maximum candidate IZOT frames to report.", 0, 16)}
                }}
            },
            {{"title", "Snapshot iZotope IPC"}, {"readOnlyHint", true}, {"openWorldHint", false}}
        },
        {
            "izotope_ipc_dump",
            "Dump iZotope IPC",
            "Write a bounded raw byte range from the attached IPC segment to a local file.",
            {
                {"type", "object"},
                {"properties", {
                    {"output_path", stringProperty("Local file path for the raw dump.")},
                    {"offset", integerProperty("Byte offset within the mapped segment.", 0, 0)},
                    {"size_bytes", integerProperty("Number of bytes to dump, capped by the backend.", 1, 65536)}
                }},
                {"required", {"output_path"}}
            },
            {{"title", "Dump iZotope IPC"}, {"readOnlyHint", true}, {"openWorldHint", false}}
        },
        {
            "izotope_ipc_capture",
            "Capture iZotope IPC Diffs",
            "Sample a bounded read-only IPC memory window and record byte changes for reverse-engineering Assistant activity.",
            {
                {"type", "object"},
                {"properties", {
                    {"offset", integerProperty("Byte offset within the mapped segment.", 0, 0)},
                    {"size_bytes", integerProperty("Number of bytes to sample, capped by the backend.", 1, 4096)},
                    {"duration_ms", integerProperty("Total live capture duration. Use 0 with baseline_base64 for one-shot diff.", 0, 2000)},
                    {"interval_ms", integerProperty("Delay between live samples.", 1, 25)},
                    {"max_changes", integerProperty("Maximum changed samples to record before stopping.", 1, 64)},
                    {"max_ranges_per_change", integerProperty("Maximum changed byte ranges stored for each changed sample.", 1, 64)},
                    {"max_frames", integerProperty("Maximum candidate IZOT frames reported per changed sample.", 0, 16)},
                    {"baseline_base64", stringProperty("Optional previous snapshot bytes for one-shot before/after diffing.")},
                    {"output_path", stringProperty("Optional local JSONL file path for changed-sample events.")},
                    {"include_changed_bytes", {
                        {"type", "boolean"},
                        {"default", false},
                        {"description", "Include previous/current hex snippets for changed byte ranges."}
                    }}
                }}
            },
            {{"title", "Capture iZotope IPC Diffs"}, {"readOnlyHint", true}, {"openWorldHint", false}}
        },
        {
            "ozone_run_assistant",
            "Run Ozone Assistant via IPC",
            "Experimental Windows-first tool that injects an AssistantRequest into a manifest-defined iZotope IPC ring buffer and waits for AssistantResult parameter decisions.",
            {
                {"type", "object"},
                {"properties", {
                    {"schema_path", stringProperty("Path to the versioned IPC schema manifest JSON. Falls back to IZOTOPE_IPC_SCHEMA_PATH.")},
                    {"segment_name", stringProperty("Explicit shared-memory segment name. Falls back to IZOTOPE_IPC_SEGMENT_NAME or manifest template + daw_process_id.")},
                    {"daw_process_id", integerProperty("Optional DAW process ID for manifest segment_name_template replacement.", 1, 0)},
                    {"ozone_instance_id", integerProperty("Optional explicit Ozone IPC instance ID.", 1, 0)},
                    {"plugin_name_query", stringProperty("Case-insensitive registry name query used when ozone_instance_id is not supplied.")},
                    {"timeout_ms", integerProperty("Maximum time to wait for AssistantResult.", 0, 10000)},
                    {"poll_interval_ms", integerProperty("Delay between AssistantResult polling attempts.", 1, 10)},
                    {"observer_id", integerProperty("Synthetic observer sender ID for AssistantRequest.", 1, 0x7fffffff)},
                    {"allow_unsafe_write", {
                        {"type", "boolean"},
                        {"default", false},
                        {"description", "Must be true, with MORE_PHI_ENABLE_IZOTOPE_IPC_WRITE=1, before any IPC write occurs."}
                    }},
                    {"apply_result", {
                        {"type", "boolean"},
                        {"default", false},
                        {"description", "Apply AssistantResult parameter decisions to the hosted Ozone plugin. Defaults to capture-only."}
                    }}
                }},
                {"required", {"allow_unsafe_write"}}
            },
            {{"title", "Run Ozone Assistant via IPC"}, {"destructiveHint", true}, {"idempotentHint", false}, {"openWorldHint", true}}
        }
    };
}

json StandaloneMcpServer::processJson(const json& request)
{
    const json id = request.contains("id") ? request["id"] : json(nullptr);
    const auto method = request.value("method", "");

    if (method.empty())
        return JsonRpc::makeError(id, -32600, "Invalid Request");

    if (!request.contains("id"))
        return json(); // notification, no response

    if (method == "initialize")
        return handleInitialize(id);
    if (method == "tools/list")
        return handleToolsList(id);
    if (method == "tools/call")
        return handleToolsCall(id, request.value("params", json::object()));

    return JsonRpc::makeError(id, -32601, "Method not found: " + method);
}

std::optional<json> StandaloneMcpServer::processLine(const std::string& line)
{
    try
    {
        auto response = processJson(json::parse(line));
        if (response.is_null())
            return std::nullopt;
        return response;
    }
    catch (...)
    {
        return JsonRpc::makeError(nullptr, -32700, "Parse error");
    }
}

void StandaloneMcpServer::run(std::istream& in, std::ostream& out)
{
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;

        const auto response = processLine(line);
        if (!response)
            continue;

        const std::lock_guard<std::mutex> guard(writeMutex);
        out << JsonRpc::serializeLine(*response);
        out.flush();
    }
}

json StandaloneMcpServer::handleInitialize(const json& id)
{
    initialized = true;
    return JsonRpc::makeResult(id, {
        {"protocolVersion", "2025-06-18"},
        {"capabilities", {{"tools", {{"listChanged", false}}}}},
        {"serverInfo", {{"name", "morephi-ozone-plugin-mcp"}, {"version", "1.0.0"}}}
    });
}

json StandaloneMcpServer::handleToolsList(const json& id) const
{
    json toolList = json::array();
    for (const auto& tool : tools)
        toolList.push_back(descriptorToJson(tool));

    return JsonRpc::makeResult(id, {{"tools", toolList}, {"nextCursor", nullptr}});
}

json StandaloneMcpServer::handleToolsCall(const json& id, const json& params)
{
    if (!params.is_object() || !params.contains("name") || !params["name"].is_string())
        return invalidParams(id, "tools/call requires a string name.");

    const auto name = params["name"].get<std::string>();
    const auto args = params.value("arguments", json::object());
    if (!args.is_object())
        return invalidParams(id, "tools/call arguments must be an object.");

    const auto toolIt = std::find_if(tools.begin(), tools.end(), [&](const ToolDescriptor& tool)
    {
        return tool.name == name;
    });
    if (toolIt == tools.end())
        return invalidParams(id, "Unknown tool: " + name);

    ToolCallOutcome outcome;

    if (name == "ozone_get_parameters")
    {
        ParameterListArgs parsed;
        parsed.query = optionalString(args, "query");
        parsed.includeValues = optionalBool(args, "include_values", true);
        outcome = backend->getParameters(parsed);
    }
    else if (name == "ozone_set_parameter")
    {
        if (!args.contains("index") || !args["index"].is_number_integer())
            return invalidParams(id, "index must be an integer.");
        if (!args.contains("value") || !args["value"].is_number())
            return invalidParams(id, "value must be a number between 0.0 and 1.0.");

        const auto index = args["index"].get<int>();
        const auto value = args["value"].get<double>();
        if (index < 0)
            return invalidParams(id, "index must be non-negative.");
        if (value < 0.0 || value > 1.0)
            return invalidParams(id, "value must be between 0.0 and 1.0.");

        outcome = backend->setParameter(index, static_cast<float>(value));
    }
    else if (name == "ozone_run_master_assistant")
    {
        RunAssistantArgs parsed;
        parsed.assistantParameterIndex = optionalInt(args, "assistant_parameter_index");
        parsed.inputAudioPath = optionalString(args, "input_audio_path");
        parsed.analysisSeconds = optionalNumber(args, "analysis_seconds", 30.0);

        if (parsed.assistantParameterIndex && *parsed.assistantParameterIndex < 0)
            return invalidParams(id, "assistant_parameter_index must be non-negative.");
        if (parsed.analysisSeconds < 0.01 || parsed.analysisSeconds > 600.0)
            return invalidParams(id, "analysis_seconds must be between 0.01 and 600.");

        outcome = backend->runMasterAssistant(parsed);
    }
    else if (name == "ozone_get_state")
    {
        outcome = backend->getState();
    }
    else if (name == "ozone_set_state")
    {
        if (!hasRequiredString(args, "state_base64"))
            return invalidParams(id, "state_base64 is required.");

        outcome = backend->setState(args["state_base64"].get<std::string>());
    }
    else if (name == "izotope_ipc_attach")
    {
        IpcAttachArgs parsed;
        parsed.segmentName = optionalString(args, "segment_name");

        if (args.contains("daw_process_id") && !optionalSize(args, "daw_process_id"))
            return invalidParams(id, "daw_process_id must be a non-negative integer.");
        if (args.contains("mapped_size_bytes") && !optionalSize(args, "mapped_size_bytes"))
            return invalidParams(id, "mapped_size_bytes must be a non-negative integer.");

        if (auto pid = optionalSize(args, "daw_process_id"))
            parsed.dawProcessId = static_cast<uint32_t>(*pid);
        if (auto size = optionalSize(args, "mapped_size_bytes"))
            parsed.mappedSizeBytes = *size;

        outcome = ipcDiscovery->attach(parsed);
    }
    else if (name == "izotope_ipc_detach")
    {
        outcome = ipcDiscovery->detach();
    }
    else if (name == "izotope_ipc_status")
    {
        outcome = ipcDiscovery->status();
    }
    else if (name == "izotope_ipc_snapshot")
    {
        if (args.contains("offset") && !optionalSize(args, "offset"))
            return invalidParams(id, "offset must be a non-negative integer.");
        if (args.contains("size_bytes") && !optionalSize(args, "size_bytes"))
            return invalidParams(id, "size_bytes must be a non-negative integer.");
        if (args.contains("max_frames") && !optionalSize(args, "max_frames"))
            return invalidParams(id, "max_frames must be a non-negative integer.");

        IpcSnapshotArgs parsed;
        if (auto offset = optionalSize(args, "offset"))
            parsed.offset = *offset;
        if (auto size = optionalSize(args, "size_bytes"))
            parsed.sizeBytes = *size;
        if (auto maxFrames = optionalSize(args, "max_frames"))
            parsed.maxFrames = *maxFrames;

        outcome = ipcDiscovery->snapshot(parsed);
    }
    else if (name == "izotope_ipc_dump")
    {
        if (!hasRequiredString(args, "output_path"))
            return invalidParams(id, "output_path is required.");
        if (args.contains("offset") && !optionalSize(args, "offset"))
            return invalidParams(id, "offset must be a non-negative integer.");
        if (args.contains("size_bytes") && !optionalSize(args, "size_bytes"))
            return invalidParams(id, "size_bytes must be a non-negative integer.");

        IpcDumpArgs parsed;
        parsed.outputPath = args["output_path"].get<std::string>();
        if (auto offset = optionalSize(args, "offset"))
            parsed.offset = *offset;
        if (auto size = optionalSize(args, "size_bytes"))
            parsed.sizeBytes = *size;

        outcome = ipcDiscovery->dump(parsed);
    }
    else if (name == "izotope_ipc_capture")
    {
        if (args.contains("offset") && !optionalSize(args, "offset"))
            return invalidParams(id, "offset must be a non-negative integer.");
        if (args.contains("size_bytes") && !optionalSize(args, "size_bytes"))
            return invalidParams(id, "size_bytes must be a non-negative integer.");
        if (args.contains("duration_ms") && !optionalSize(args, "duration_ms"))
            return invalidParams(id, "duration_ms must be a non-negative integer.");
        if (args.contains("interval_ms") && !optionalSize(args, "interval_ms"))
            return invalidParams(id, "interval_ms must be a non-negative integer.");
        if (args.contains("max_changes") && !optionalSize(args, "max_changes"))
            return invalidParams(id, "max_changes must be a non-negative integer.");
        if (args.contains("max_ranges_per_change") && !optionalSize(args, "max_ranges_per_change"))
            return invalidParams(id, "max_ranges_per_change must be a non-negative integer.");
        if (args.contains("max_frames") && !optionalSize(args, "max_frames"))
            return invalidParams(id, "max_frames must be a non-negative integer.");

        IpcCaptureArgs parsed;
        if (auto offset = optionalSize(args, "offset"))
            parsed.offset = *offset;
        if (auto size = optionalSize(args, "size_bytes"))
            parsed.sizeBytes = *size;
        if (auto duration = optionalSize(args, "duration_ms"))
            parsed.durationMs = *duration;
        if (auto interval = optionalSize(args, "interval_ms"))
            parsed.intervalMs = *interval;
        if (auto maxChanges = optionalSize(args, "max_changes"))
            parsed.maxChanges = *maxChanges;
        if (auto maxRanges = optionalSize(args, "max_ranges_per_change"))
            parsed.maxRangesPerChange = *maxRanges;
        if (auto maxFrames = optionalSize(args, "max_frames"))
            parsed.maxFrames = *maxFrames;

        parsed.baselineBase64 = optionalString(args, "baseline_base64");
        parsed.outputPath = optionalString(args, "output_path");
        parsed.includeChangedBytes = optionalBool(args, "include_changed_bytes", false);

        outcome = ipcDiscovery->capture(parsed);
    }
    else if (name == "ozone_run_assistant")
    {
        if (args.contains("daw_process_id") && !optionalSize(args, "daw_process_id"))
            return invalidParams(id, "daw_process_id must be a non-negative integer.");
        if (args.contains("ozone_instance_id") && !optionalSize(args, "ozone_instance_id"))
            return invalidParams(id, "ozone_instance_id must be a non-negative integer.");
        if (args.contains("timeout_ms") && !optionalSize(args, "timeout_ms"))
            return invalidParams(id, "timeout_ms must be a non-negative integer.");
        if (args.contains("poll_interval_ms") && !optionalSize(args, "poll_interval_ms"))
            return invalidParams(id, "poll_interval_ms must be a non-negative integer.");
        if (args.contains("observer_id") && !optionalSize(args, "observer_id"))
            return invalidParams(id, "observer_id must be a non-negative integer.");
        if (args.contains("allow_unsafe_write") && !args["allow_unsafe_write"].is_boolean())
            return invalidParams(id, "allow_unsafe_write must be a boolean.");
        if (args.contains("apply_result") && !args["apply_result"].is_boolean())
            return invalidParams(id, "apply_result must be a boolean.");

        IpcAssistantRunArgs parsed;
        parsed.schemaPath = optionalString(args, "schema_path");
        parsed.segmentName = optionalString(args, "segment_name");
        if (auto pid = optionalSize(args, "daw_process_id"))
            parsed.dawProcessId = static_cast<uint32_t>(*pid);
        if (auto ozoneId = optionalSize(args, "ozone_instance_id"))
            parsed.ozoneInstanceId = static_cast<uint32_t>(*ozoneId);
        if (auto query = optionalString(args, "plugin_name_query"); query && !query->empty())
            parsed.pluginNameQuery = *query;
        if (auto timeout = optionalSize(args, "timeout_ms"))
            parsed.timeoutMs = *timeout;
        if (auto interval = optionalSize(args, "poll_interval_ms"))
            parsed.pollIntervalMs = *interval;
        if (auto observer = optionalSize(args, "observer_id"))
            parsed.observerId = static_cast<uint32_t>(*observer);
        parsed.allowUnsafeWrite = optionalBool(args, "allow_unsafe_write", false);

        outcome = ipcAssistant->runAssistant(parsed);
        if (!outcome.isError && optionalBool(args, "apply_result", false))
        {
            AssistantParameterApplyArgs applyArgs;
            auto parseOutcome = parseAssistantApplyArgs(outcome.body, applyArgs);
            if (parseOutcome.isError)
            {
                parseOutcome.body["parameters"] = outcome.body.value("parameters", json::array());
                outcome = std::move(parseOutcome);
            }
            else
            {
                auto applyOutcome = backend->applyAssistantParameters(applyArgs);
                outcome.body["apply_result"] = applyOutcome.body.value("apply_result", json::object());
                if (applyOutcome.isError)
                {
                    outcome.body["success"] = false;
                    outcome.body["error"] = applyOutcome.body.value("error", "assistant_apply_failed");
                    outcome.isError = true;
                }
            }
        }
    }

    return JsonRpc::makeToolResult(id, outcome.body, outcome.isError);
}

json StandaloneMcpServer::descriptorToJson(const ToolDescriptor& tool) const
{
    return {
        {"name", tool.name},
        {"title", tool.title},
        {"description", tool.description},
        {"inputSchema", tool.inputSchema},
        {"annotations", tool.annotations}
    };
}

json StandaloneMcpServer::invalidParams(const json& id, const std::string& message)
{
    return JsonRpc::makeError(id, -32602, message);
}

} // namespace more_phi::standalone_mcp
