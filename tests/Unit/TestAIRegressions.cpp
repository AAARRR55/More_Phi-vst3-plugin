/*
 * More-Phi — AI/Concurrency Regression Tests
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/MCPToolsExtended.h"
#include "AI/MCPToolHandler.h"
#include "AI/SemanticPluginProfile.h"
#include "AI/TokenOptimizer.h"
#include "Core/ParameterClassifier.h"
#include "Host/IPluginHostManager.h"
#include "Plugin/PluginProcessor.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <initializer_list>

using Catch::Approx;
using namespace more_phi;

namespace {

class FakeParameterBridge final : public IParameterBridge
{
public:
    FakeParameterBridge(std::vector<juce::String> names,
                        std::vector<float> values,
                        std::vector<bool> discrete)
        : names_(std::move(names))
        , values_(std::move(values))
        , discrete_(std::move(discrete))
    {
    }

    int getParameterCount() const override
    {
        return static_cast<int>(values_.size());
    }

    float getParameterNormalized(int index) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= values_.size())
            return 0.0f;
        return values_[static_cast<size_t>(index)];
    }

    void setParameterNormalized(int index, float value) override
    {
        if (index >= 0 && static_cast<size_t>(index) < values_.size())
            values_[static_cast<size_t>(index)] = value;
    }

    juce::String getParameterName(int index) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= names_.size())
            return {};
        return names_[static_cast<size_t>(index)];
    }

    void applyParameterState(const std::vector<float>& values) override
    {
        values_ = values;
    }

    void applyParameterState(const float* values, int count) override
    {
        if (values == nullptr || count <= 0)
            return;
        values_.assign(values, values + count);
    }

    std::vector<float> captureParameterState() const override
    {
        return values_;
    }

    bool isDiscrete(int index) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= discrete_.size())
            return false;
        return discrete_[static_cast<size_t>(index)];
    }

    std::vector<bool> getDiscreteMap() const override
    {
        return discrete_;
    }

    juce::String getParameterLabel(int) const override { return {}; }
    juce::String getParameterDisplayValue(int index) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= values_.size())
            return {};
        return juce::String(values_[static_cast<size_t>(index)], 3);
    }
    float getParameterDefault(int) const override { return 0.5f; }
    juce::StringArray getParameterValueStrings(int) const override { return {}; }
    juce::String getParameterStableID(int index) const override
    {
        if (index < 0 || static_cast<size_t>(index) >= names_.size())
            return {};
        return names_[static_cast<size_t>(index)];
    }
    int getParameterNumSteps(int) const override { return 0; }

private:
    std::vector<juce::String> names_;
    std::vector<float> values_;
    std::vector<bool> discrete_;
};

std::vector<ParameterBridge::ParameterDescriptor> makeTestDescriptors(std::initializer_list<const char*> names)
{
    std::vector<ParameterBridge::ParameterDescriptor> descriptors;
    descriptors.reserve(names.size());

    int index = 0;
    for (const auto* name : names)
    {
        ParameterBridge::ParameterDescriptor descriptor;
        descriptor.index = index;
        descriptor.stableId = "test-" + juce::String(index);
        descriptor.name = name;
        descriptor.value = 0.5f;
        descriptor.displayValue = "0.500";
        descriptor.defaultValue = 0.5f;
        descriptors.push_back(descriptor);
        ++index;
    }

    return descriptors;
}

void fillCommandQueueLeaving(MorePhiProcessor& processor, int freeSlots)
{
    const int targetQueued = std::max(0, processor.getCommandQueueCapacity() - 1 - freeSlots);
    for (int i = 0; i < targetQueued; ++i)
    {
        REQUIRE(processor.enqueueParameterSet(0, 0.5f,
                                              MorePhiProcessor::ParameterEditSource::MCP,
                                              true));
    }
}

} // namespace

TEST_CASE("SemanticPluginProfile classifies EQ and limiter controls", "[unit][ai][semantic]")
{
    auto descriptors = makeTestDescriptors({
        "Band 1 Frequency",
        "Band 1 Gain",
        "Band 1 Q",
        "Bypass",
        "Limiter Ceiling",
        "Stereo Width"
    });
    descriptors[3].boolean = true;
    descriptors[3].discrete = true;
    descriptors[3].numSteps = 2;

    const auto controls = SemanticPluginProfile::classify(descriptors);

    const auto* frequency = SemanticPluginProfile::findControl(controls, "eq.band_1.frequency");
    REQUIRE(frequency != nullptr);
    REQUIRE(frequency->parameterIndex == 0);
    REQUIRE(frequency->role == "eq_band_frequency");
    REQUIRE(frequency->unit == "Hz");
    REQUIRE(frequency->safety == "safe");

    const auto* gain = SemanticPluginProfile::findControl(controls, "eq.band_1.gain");
    REQUIRE(gain != nullptr);
    REQUIRE(gain->parameterIndex == 1);
    REQUIRE(gain->role == "eq_band_gain");
    REQUIRE(gain->unit == "dB");
    REQUIRE(gain->safety == "safe");
    REQUIRE(gain->maxStepDb == Approx(3.0f));
    REQUIRE(gain->minDeltaDb == Approx(-6.0f));
    REQUIRE(gain->maxDeltaDb == Approx(3.0f));

    const auto* q = SemanticPluginProfile::findControl(controls, "eq.band_1.q");
    REQUIRE(q != nullptr);
    REQUIRE(q->parameterIndex == 2);
    REQUIRE(q->role == "eq_band_q");
    REQUIRE(q->safety == "safe");

    const auto* bypass = SemanticPluginProfile::findControl(controls, "locked.bypass");
    REQUIRE(bypass != nullptr);
    REQUIRE(bypass->parameterIndex == 3);
    REQUIRE(bypass->safety == "locked");

    const auto* ceiling = SemanticPluginProfile::findControl(controls, "limiter.ceiling");
    REQUIRE(ceiling != nullptr);
    REQUIRE(ceiling->parameterIndex == 4);
    REQUIRE(ceiling->role == "limiter_ceiling");
    REQUIRE(ceiling->safety == "caution");

    const auto* width = SemanticPluginProfile::findControl(controls, "imager.width");
    REQUIRE(width != nullptr);
    REQUIRE(width->parameterIndex == 5);
    REQUIRE(width->role == "imager_width");
    REQUIRE(width->safety == "caution");

    const auto json = SemanticPluginProfile::controlsToJson(descriptors, controls);
    REQUIRE(json.is_array());
    REQUIRE(json.size() == descriptors.size());
    REQUIRE(json[3]["parameter"]["boolean"].get<bool>());
    REQUIRE(json[3]["safety"].get<std::string>() == "locked");
}

TEST_CASE("SemanticPluginProfile semantic IDs ignore display values", "[unit][ai][semantic]")
{
    auto descriptors = makeTestDescriptors({
        "Frequency",
        "Band 999999999999999999999999999999999 Frequency",
        "Node 2 Gain"
    });
    descriptors[0].displayValue = "1000 Hz";
    descriptors[1].displayValue = "250 Hz";

    const auto controls = SemanticPluginProfile::classify(descriptors);

    const auto* frequency = SemanticPluginProfile::findControl(controls, "eq.frequency");
    REQUIRE(frequency != nullptr);
    REQUIRE(frequency->parameterIndex == 0);
    REQUIRE(controls[1].semanticId == "eq.frequency.param_1");
    REQUIRE(controls[1].parameterIndex == 1);
    REQUIRE(SemanticPluginProfile::findControl(controls, "eq.band_1000.frequency") == nullptr);
    REQUIRE(SemanticPluginProfile::findControl(controls, "eq.band_999999999999999999999999999999999.frequency") == nullptr);

    const auto* nodeGain = SemanticPluginProfile::findControl(controls, "eq.band_2.gain");
    REQUIRE(nodeGain != nullptr);
    REQUIRE(nodeGain->parameterIndex == 2);
}

TEST_CASE("SemanticPluginProfile disambiguates duplicate semantic IDs", "[unit][ai][semantic]")
{
    const auto controls = SemanticPluginProfile::classify(makeTestDescriptors({
        "Gain",
        "Gain",
        "Frequency",
        "Frequency"
    }));

    REQUIRE(controls.size() == 4);
    REQUIRE(controls[0].semanticId == "eq.gain");
    REQUIRE(controls[1].semanticId == "eq.gain.param_1");
    REQUIRE(controls[2].semanticId == "eq.frequency");
    REQUIRE(controls[3].semanticId == "eq.frequency.param_3");
}

TEST_CASE("ParameterBridge getParameterDescriptor uses test descriptors", "[unit][ai][semantic]")
{
    MorePhiProcessor processor;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 1 Gain"}));

    auto descriptor = processor.getParameterBridge().getParameterDescriptor(0);

    REQUIRE(descriptor.index == 0);
    REQUIRE(descriptor.name == "Band 1 Gain");
}

TEST_CASE("ParameterBridge display value samples test descriptors with clamping", "[unit][ai][semantic]")
{
    MorePhiProcessor processor;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 1 Gain"}));

    REQUIRE(processor.getParameterBridge().getParameterDisplayValueAtNormalized(0, -0.25f) == "0.000");
    REQUIRE(processor.getParameterBridge().getParameterDisplayValueAtNormalized(0, 0.25f) == "0.250");
    REQUIRE(processor.getParameterBridge().getParameterDisplayValueAtNormalized(0, 1.25f) == "1.000");
    REQUIRE(processor.getParameterBridge().getParameterDisplayValueAtNormalized(1, 0.25f).isEmpty());
}

TEST_CASE("MCP tool list exposes semantic profile tools", "[unit][ai][semantic][mcp]")
{
    const auto tools = MCPToolHandler::getToolList();
    REQUIRE(tools.contains("plugin_profile.describe_semantics"));
    REQUIRE(tools.contains("plugin_profile.apply_safe_action"));
    REQUIRE(tools.contains("plugin_profile.restore_safe_snapshot"));
}

TEST_CASE("MCP plugin profile audit includes semantic controls", "[unit][ai][semantic][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 1 Frequency", "Band 1 Gain", "Band 1 Q"}));

    const auto response = MCPToolHandler::handle("plugin_profile.audit_parameters", juce::var(), processor, identity);
    const auto parsed = nlohmann::json::parse(response.toStdString());

    REQUIRE(parsed.value("success", false));
    REQUIRE(parsed.value("semantic_control_count", 0) >= 3);
}

TEST_CASE("MCP safe action rejects locked controls", "[unit][ai][semantic][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    auto descriptors = makeTestDescriptors({"Bypass"});
    descriptors[0].boolean = true;
    descriptors[0].discrete = true;
    descriptors[0].numSteps = 2;
    processor.getParameterBridge().setParameterDescriptorsForTesting(std::move(descriptors));

    juce::DynamicObject::Ptr action = new juce::DynamicObject();
    action->setProperty("type", "set_semantic_normalized");
    action->setProperty("semantic_id", "locked.bypass");
    action->setProperty("normalized_value", 1.0);

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("action", juce::var(action.get()));
    req->setProperty("allow_caution", false);
    req->setProperty("dry_run", false);

    const auto response = MCPToolHandler::handle("plugin_profile.apply_safe_action", juce::var(req.get()), processor, identity);
    const auto parsed = nlohmann::json::parse(response.toStdString());

    REQUIRE_FALSE(parsed.value("success", true));
    REQUIRE(parsed.value("error", std::string{}) == "control_locked");
}

TEST_CASE("MCP safe normalized action queues safe control and returns snapshot", "[unit][ai][semantic][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 1 Gain"}));

    juce::DynamicObject::Ptr action = new juce::DynamicObject();
    action->setProperty("type", "set_semantic_normalized");
    action->setProperty("semantic_id", "eq.band_1.gain");
    action->setProperty("normalized_value", 0.48);

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("action", juce::var(action.get()));

    const auto response = MCPToolHandler::handle("plugin_profile.apply_safe_action", juce::var(req.get()), processor, identity);
    const auto parsed = nlohmann::json::parse(response.toStdString());

    REQUIRE(parsed.value("success", false));
    REQUIRE(parsed.value("queued", 0) == 1);
    const auto snapshotId = parsed.value("snapshot_id", std::string{});
    REQUIRE(snapshotId.rfind("safe_action_", 0) == 0);
}

TEST_CASE("MCP safe action rejects when parameter edits are pending", "[unit][ai][semantic][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 1 Gain"}));

    REQUIRE(processor.enqueueParameterSet(0, 0.25f,
                                          MorePhiProcessor::ParameterEditSource::MCP,
                                          true));

    juce::DynamicObject::Ptr action = new juce::DynamicObject();
    action->setProperty("type", "set_semantic_normalized");
    action->setProperty("semantic_id", "eq.band_1.gain");
    action->setProperty("normalized_value", 0.48);

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("action", juce::var(action.get()));

    const auto response = MCPToolHandler::handle("plugin_profile.apply_safe_action", juce::var(req.get()), processor, identity);
    const auto parsed = nlohmann::json::parse(response.toStdString());

    REQUIRE_FALSE(parsed.value("success", true));
    REQUIRE(parsed.value("error", std::string{}) == "pending_parameter_edits");
    REQUIRE(parsed.value("queued", -1) == 0);
    REQUIRE(parsed.value("pending", 0) >= 1);
}

TEST_CASE("MCP restores safe action snapshot through parameter queue", "[unit][ai][semantic][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 1 Gain"}));

    juce::DynamicObject::Ptr action = new juce::DynamicObject();
    action->setProperty("type", "set_semantic_normalized");
    action->setProperty("semantic_id", "eq.band_1.gain");
    action->setProperty("normalized_value", 0.48);

    juce::DynamicObject::Ptr applyReq = new juce::DynamicObject();
    applyReq->setProperty("action", juce::var(action.get()));

    const auto applyResponse = MCPToolHandler::handle("plugin_profile.apply_safe_action", juce::var(applyReq.get()), processor, identity);
    const auto applyParsed = nlohmann::json::parse(applyResponse.toStdString());
    REQUIRE(applyParsed.value("success", false));
    const auto snapshotId = applyParsed.value("snapshot_id", std::string{});
    REQUIRE_FALSE(snapshotId.empty());

    juce::DynamicObject::Ptr restoreReq = new juce::DynamicObject();
    restoreReq->setProperty("snapshot_id", juce::String(snapshotId));

    const auto restoreResponse = MCPToolHandler::handle("plugin_profile.restore_safe_snapshot", juce::var(restoreReq.get()), processor, identity);
    const auto restoreParsed = nlohmann::json::parse(restoreResponse.toStdString());

    REQUIRE(restoreParsed.value("success", false));
    REQUIRE(restoreParsed.value("snapshot_id", std::string{}) == snapshotId);
    REQUIRE(restoreParsed.value("queued", 0) >= 1);
}

TEST_CASE("MCP safe action snapshot restore rejects changed parameter layout", "[unit][ai][semantic][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 1 Gain"}));

    juce::DynamicObject::Ptr action = new juce::DynamicObject();
    action->setProperty("type", "set_semantic_normalized");
    action->setProperty("semantic_id", "eq.band_1.gain");
    action->setProperty("normalized_value", 0.48);

    juce::DynamicObject::Ptr applyReq = new juce::DynamicObject();
    applyReq->setProperty("action", juce::var(action.get()));

    const auto applyResponse = MCPToolHandler::handle("plugin_profile.apply_safe_action", juce::var(applyReq.get()), processor, identity);
    const auto applyParsed = nlohmann::json::parse(applyResponse.toStdString());
    REQUIRE(applyParsed.value("success", false));
    const auto snapshotId = applyParsed.value("snapshot_id", std::string{});
    REQUIRE_FALSE(snapshotId.empty());

    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Band 2 Gain"}));

    juce::DynamicObject::Ptr restoreReq = new juce::DynamicObject();
    restoreReq->setProperty("snapshot_id", juce::String(snapshotId));

    const auto restoreResponse = MCPToolHandler::handle("plugin_profile.restore_safe_snapshot", juce::var(restoreReq.get()), processor, identity);
    const auto restoreParsed = nlohmann::json::parse(restoreResponse.toStdString());

    REQUIRE_FALSE(restoreParsed.value("success", true));
    REQUIRE(restoreParsed.value("error", std::string{}) == "snapshot_context_mismatch");
    REQUIRE(restoreParsed.value("queued", -1) == 0);
}

TEST_CASE("ParameterClassifier token estimation/optimization paths do not deadlock", "[unit][ai][classifier]")
{
    FakeParameterBridge bridge(
        {"Cutoff", "Waveform", "Bypass", "Mix"},
        {0.25f, 0.1f, 0.0f, 0.5f},
        {false, true, true, false});

    ParameterClassifier classifier;
    classifier.analyzeParameters(bridge);

    const auto estimate = classifier.estimateTokens(true);
    REQUIRE(estimate.totalTokens >= estimate.systemPromptTokens);

    const auto optimized = classifier.optimizeForTokenBudget(512);
    REQUIRE_FALSE(optimized.empty());
}

TEST_CASE("TokenOptimizer compression bounds and rate-limit bookkeeping are safe", "[unit][ai][token]")
{
    TokenOptimizer optimizer;

    const auto compressed = optimizer.compressParameters({0.12f, 0.34f}, {1200, 1201});
    REQUIRE_FALSE(compressed.compressedData.empty());
    REQUIRE(compressed.compressedTokens > 0);

    optimizer.setRateLimit(1);
    REQUIRE(optimizer.canMakeRequest());
    REQUIRE(optimizer.tryConsumeRequestSlot());
    REQUIRE_FALSE(optimizer.canMakeRequest());
    REQUIRE(optimizer.getTimeUntilNextRequest() >= 0.0f);
}

TEST_CASE("MCP morph compatibility returns computed non-placeholder output", "[unit][ai][mcp]")
{
    FakeParameterBridge bridge(
        {"Cutoff", "Mode", "Mix"},
        {0.1f, 0.0f, 0.2f},
        {false, true, false});

    ParameterClassifier classifier;
    classifier.analyzeParameters(bridge);

    MorePhiProcessor processor;
    processor.getSnapshotBank().prepare(3);
    processor.getSnapshotBank().captureValues(0, {0.1f, 0.0f, 0.2f});
    processor.getSnapshotBank().captureValues(1, {0.9f, 1.0f, 0.8f});

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("snapshot_a", 0);
    req->setProperty("snapshot_b", 1);

    const auto response = MCPToolsExtended::getMorphCompatibility(juce::var(req.get()), processor, classifier);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());

    const auto success = static_cast<bool>(parsedVar.getProperty("success", false));
    REQUIRE(success);
    REQUIRE(static_cast<int>(parsedVar.getProperty("problematic_parameter_count", 0)) > 0);
    REQUIRE(static_cast<float>(parsedVar.getProperty("compatibility_score", 1.0f)) < 1.0f);
}

TEST_CASE("Processor external edit hold persists until morph position moves", "[unit][ai][processor]")
{
    MorePhiProcessor processor;
    processor.testResizeExternalEditHolds(4);

    processor.testMarkExternalEditHold(1, 0.25f, 0.75f, 0.10f);
    REQUIRE(processor.testIsExternalEditHeld(1));

    REQUIRE(processor.testShouldKeepExternalEditHold(1, 0.254f, 0.754f, 0.104f));
    REQUIRE(processor.testIsExternalEditHeld(1));

    REQUIRE_FALSE(processor.testShouldKeepExternalEditHold(1, 0.262f, 0.75f, 0.10f));
    REQUIRE_FALSE(processor.testIsExternalEditHeld(1));
}

TEST_CASE("Processor snapshot recall marker clears external edit holds", "[unit][ai][processor]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(44100.0, 64);
    processor.getSnapshotBank().captureValues(0, {0.2f, 0.8f});

    processor.testMarkExternalEditHold(1, 0.5f, 0.5f, 0.0f);
    REQUIRE(processor.testIsExternalEditHeld(1));
    REQUIRE(processor.recallSnapshotQueued(0));

    juce::AudioBuffer<float> buffer(2, 64);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);

    REQUIRE_FALSE(processor.testIsExternalEditHeld(1));
    processor.releaseResources();
}

TEST_CASE("Processor parameter command queue reports full", "[unit][ai][processor]")
{
    MorePhiProcessor processor;

    int queued = 0;
    while (processor.enqueueParameterSet(0, 0.5f,
                                         MorePhiProcessor::ParameterEditSource::MCP,
                                         true))
    {
        ++queued;
    }

    REQUIRE(queued == processor.getCommandQueueCapacity() - 1);
    REQUIRE_FALSE(processor.enqueueParameterSet(0, 0.5f,
                                                MorePhiProcessor::ParameterEditSource::MCP,
                                                true));
}

TEST_CASE("MCP set_parameter reports queue_full when resolved edit cannot be queued", "[unit][ai][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain"}));

    fillCommandQueueLeaving(processor, 0);

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("index", 0);
    req->setProperty("value", 0.5);

    const auto response = MCPToolHandler::handle("set_parameter", juce::var(req.get()), processor, identity);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());

    REQUIRE_FALSE(static_cast<bool>(parsedVar.getProperty("success", true)));
    REQUIRE(parsedVar.getProperty("error", "").toString() == "queue_full");
    REQUIRE(static_cast<int>(parsedVar.getProperty("queued", -1)) == 0);
    REQUIRE(static_cast<int>(parsedVar.getProperty("rejected", -1)) == 0);
}

TEST_CASE("MCP set_parameters_batch reports partial queue_full accounting", "[unit][ai][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain", "Cutoff"}));

    fillCommandQueueLeaving(processor, 1);

    juce::Array<juce::var> updates;
    for (int index = 0; index < 2; ++index)
    {
        juce::DynamicObject::Ptr update = new juce::DynamicObject();
        update->setProperty("index", index);
        update->setProperty("value", 0.5);
        updates.add(juce::var(update.get()));
    }

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("parameters", updates);

    const auto response = MCPToolHandler::handle("set_parameters_batch", juce::var(req.get()), processor, identity);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());

    REQUIRE_FALSE(static_cast<bool>(parsedVar.getProperty("success", true)));
    REQUIRE(parsedVar.getProperty("error", "").toString() == "queue_full");
    REQUIRE(static_cast<int>(parsedVar.getProperty("queued", -1)) == 1);
    REQUIRE(static_cast<int>(parsedVar.getProperty("queueFailures", -1)) == 1);
    REQUIRE(static_cast<int>(parsedVar.getProperty("rejected", -1)) == 0);
}

TEST_CASE("MCP optimized set reports queue_full when resolved edit cannot be queued", "[unit][ai][mcp]")
{
    MorePhiProcessor processor;
    TokenOptimizer optimizer;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain"}));

    fillCommandQueueLeaving(processor, 0);

    juce::Array<juce::var> updates;
    juce::DynamicObject::Ptr update = new juce::DynamicObject();
    update->setProperty("index", 0);
    update->setProperty("value", 0.5);
    updates.add(juce::var(update.get()));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("parameters", updates);

    const auto response = MCPToolsExtended::setParametersOptimized(juce::var(req.get()), processor, optimizer);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());

    REQUIRE_FALSE(static_cast<bool>(parsedVar.getProperty("success", true)));
    REQUIRE(parsedVar.getProperty("error", "").toString() == "queue_full");
    REQUIRE(static_cast<int>(parsedVar.getProperty("queued_count", -1)) == 0);
    REQUIRE(static_cast<int>(parsedVar.getProperty("queue_failures", -1)) == 1);
    REQUIRE(static_cast<int>(parsedVar.getProperty("rejected_count", -1)) == 0);
}

TEST_CASE("MCP optimized set reports when no parameter edits were queued", "[unit][ai][mcp]")
{
    MorePhiProcessor processor;
    TokenOptimizer optimizer;

    juce::Array<juce::var> updates;
    juce::DynamicObject::Ptr update = new juce::DynamicObject();
    update->setProperty("index", 0);
    update->setProperty("value", 0.5);
    updates.add(juce::var(update.get()));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("parameters", updates);

    const auto response = MCPToolsExtended::setParametersOptimized(juce::var(req.get()), processor, optimizer);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());

    REQUIRE_FALSE(static_cast<bool>(parsedVar.getProperty("success", true)));
    REQUIRE(static_cast<int>(parsedVar.getProperty("queued_count", -1)) == 0);
    REQUIRE(static_cast<int>(parsedVar.getProperty("rejected_count", -1)) == 1);
    REQUIRE(parsedVar.getProperty("error", "").toString() == "no_parameters_queued");
}

TEST_CASE("MCP set_parameter reports rejected edits when no parameter resolves", "[unit][ai][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("index", 0);
    req->setProperty("value", 0.5);

    const auto response = MCPToolHandler::handle("set_parameter", juce::var(req.get()), processor, identity);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());

    REQUIRE_FALSE(static_cast<bool>(parsedVar.getProperty("success", true)));
    REQUIRE(static_cast<int>(parsedVar.getProperty("queued", -1)) == 0);
    REQUIRE(static_cast<int>(parsedVar.getProperty("rejected", -1)) == 1);
}

TEST_CASE("MCP set_parameters_batch reports all-rejected batches honestly", "[unit][ai][mcp]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;

    juce::Array<juce::var> updates;
    juce::DynamicObject::Ptr update = new juce::DynamicObject();
    update->setProperty("index", 0);
    update->setProperty("value", 0.5);
    updates.add(juce::var(update.get()));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("parameters", updates);

    const auto response = MCPToolHandler::handle("set_parameters_batch", juce::var(req.get()), processor, identity);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());

    REQUIRE_FALSE(static_cast<bool>(parsedVar.getProperty("success", true)));
    REQUIRE(static_cast<int>(parsedVar.getProperty("queued", -1)) == 0);
    REQUIRE(static_cast<int>(parsedVar.getProperty("rejected", -1)) == 1);
    REQUIRE(parsedVar.getProperty("error", "").toString() == "no_parameters_queued");
}

TEST_CASE("MCP set_morph_position survives the following APVTS processBlock sync", "[unit][ai][mcp][morph]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.prepareToPlay(48000.0, 64);

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("x", 0.2);
    req->setProperty("y", 0.8);
    req->setProperty("fader", 0.35);

    const auto response = MCPToolHandler::handle("set_morph_position", juce::var(req.get()), processor, identity);
    const auto parsedVar = juce::JSON::parse(response);
    REQUIRE_FALSE(parsedVar.isVoid());
    REQUIRE(static_cast<bool>(parsedVar.getProperty("success", false)));

    juce::AudioBuffer<float> buffer(2, 64);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);

    REQUIRE(processor.getMorphX() == Approx(0.2f).margin(0.001f));
    REQUIRE(processor.getMorphY() == Approx(0.8f).margin(0.001f));
    REQUIRE(processor.getFaderPos() == Approx(0.35f).margin(0.001f));

    auto* rawX = processor.getAPVTS().getRawParameterValue("morphX");
    auto* rawY = processor.getAPVTS().getRawParameterValue("morphY");
    auto* rawFader = processor.getAPVTS().getRawParameterValue("faderPos");
    REQUIRE(rawX != nullptr);
    REQUIRE(rawY != nullptr);
    REQUIRE(rawFader != nullptr);
    REQUIRE(rawX->load(std::memory_order_relaxed) == Approx(0.2f).margin(0.001f));
    REQUIRE(rawY->load(std::memory_order_relaxed) == Approx(0.8f).margin(0.001f));
    REQUIRE(rawFader->load(std::memory_order_relaxed) == Approx(0.35f).margin(0.001f));

    processor.releaseResources();
}

TEST_CASE("MCP tool list includes diagnose_parameter_pipeline", "[unit][ai][mcp][pipeline]")
{
    const auto tools = MCPToolHandler::getToolList();
    REQUIRE(tools.contains("diagnose_parameter_pipeline"));
}

TEST_CASE("diagnose_parameter_pipeline reports blocked stages when no plugin loaded", "[unit][ai][mcp][pipeline]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;

    const auto response = MCPToolHandler::handle("diagnose_parameter_pipeline", juce::var(), processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());
    REQUIRE(static_cast<bool>(parsed.getProperty("success", false)));

    const auto stage2 = parsed.getProperty("stage2_parameterResolution", {});
    REQUIRE(stage2.getProperty("status", "").toString() == "blocked");
    REQUIRE(static_cast<int>(stage2.getProperty("parameterCount", -1)) == 0);

    const auto stage4 = parsed.getProperty("stage4_flush", {});
    REQUIRE_FALSE(static_cast<bool>(stage4.getProperty("pluginAvailable", true)));

    const auto stage6 = parsed.getProperty("stage6_drainWrite", {});
    REQUIRE(stage6.getProperty("status", "").toString() == "no_parameters");
}

TEST_CASE("diagnose_parameter_pipeline reports healthy stages with test descriptors", "[unit][ai][mcp][pipeline]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.prepareToPlay(44100.0, 64);
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain", "Cutoff", "Resonance"}));

    const auto response = MCPToolHandler::handle("diagnose_parameter_pipeline", juce::var(), processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());
    REQUIRE(static_cast<bool>(parsed.getProperty("success", false)));

    const auto stage3 = parsed.getProperty("stage3_commandQueue", {});
    REQUIRE(static_cast<bool>(stage3.getProperty("healthy", false)));
    REQUIRE(static_cast<int>(stage3.getProperty("usagePercent", -1)) == 0);

    const auto stage5 = parsed.getProperty("stage5_processBlock", {});
    REQUIRE(static_cast<bool>(stage5.getProperty("isPrepared", false)));
    REQUIRE_FALSE(static_cast<bool>(stage5.getProperty("isRestoring", true)));
    REQUIRE(stage5.getProperty("status", "").toString() == "ok");

    const auto stage7 = parsed.getProperty("stage7_morphOverwrite", {});
    REQUIRE(static_cast<int>(stage7.getProperty("snapshotsOccupied", -1)) == 0);
    REQUIRE(stage7.getProperty("status", "").toString() == "ok_no_morph");

    processor.releaseResources();
}

TEST_CASE("diagnose_parameter_pipeline reports queue health under load", "[unit][ai][mcp][pipeline]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain"}));

    fillCommandQueueLeaving(processor, 0);

    const auto response = MCPToolHandler::handle("diagnose_parameter_pipeline", juce::var(), processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());

    const auto stage3 = parsed.getProperty("stage3_commandQueue", {});
    REQUIRE_FALSE(static_cast<bool>(stage3.getProperty("healthy", true)));
    REQUIRE(static_cast<int>(stage3.getProperty("usagePercent", -1)) > 90);
    REQUIRE(stage3.getProperty("status", "").toString() == "warning_high_usage");
}

TEST_CASE("diagnose_parameter_pipeline param_index reports per-parameter state", "[unit][ai][mcp][pipeline]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain", "Cutoff"}));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("index", 1);

    const auto response = MCPToolHandler::handle("diagnose_parameter_pipeline", juce::var(req.get()), processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());

    const auto paramDiag = parsed.getProperty("parameterDiagnostic", {});
    REQUIRE_FALSE(paramDiag.isVoid());
    REQUIRE(static_cast<int>(paramDiag.getProperty("index", -1)) == 1);
    REQUIRE(static_cast<bool>(paramDiag.getProperty("exists", false)));
    REQUIRE(paramDiag.getProperty("name", "").toString() == "Cutoff");
}

TEST_CASE("diagnose_parameter_pipeline param_index reports nonexistent for out-of-range", "[unit][ai][mcp][pipeline]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain"}));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("index", 999);

    const auto response = MCPToolHandler::handle("diagnose_parameter_pipeline", juce::var(req.get()), processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());

    const auto paramDiag = parsed.getProperty("parameterDiagnostic", {});
    REQUIRE_FALSE(paramDiag.isVoid());
    REQUIRE_FALSE(static_cast<bool>(paramDiag.getProperty("exists", true)));
}

TEST_CASE("diagnose_parameter_pipeline reports live edit holds when active", "[unit][ai][mcp][pipeline]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.prepareToPlay(44100.0, 64);
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain", "Cutoff"}));

    processor.testResizeExternalEditHolds(2);
    processor.testMarkExternalEditHold(0, 0.5f, 0.5f, 0.0f);

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("index", 0);

    const auto response = MCPToolHandler::handle("diagnose_parameter_pipeline", juce::var(req.get()), processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());

    const auto stage7 = parsed.getProperty("stage7_morphOverwrite", {});
    REQUIRE(static_cast<int>(stage7.getProperty("liveEditHoldsActive", -1)) >= 1);

    const auto paramDiag = parsed.getProperty("parameterDiagnostic", {});
    REQUIRE(static_cast<bool>(paramDiag.getProperty("liveEditHeld", false)));

    processor.releaseResources();
}

TEST_CASE("MCP set_parameter flush reports pluginUnavailable when no hosted plugin", "[unit][ai][mcp][pipeline]")
{
    MorePhiProcessor processor;
    InstanceIdentity identity;
    processor.prepareToPlay(44100.0, 64);
    processor.getParameterBridge().setParameterDescriptorsForTesting(
        makeTestDescriptors({"Gain"}));

    juce::DynamicObject::Ptr req = new juce::DynamicObject();
    req->setProperty("index", 0);
    req->setProperty("value", 0.75);

    const auto response = MCPToolHandler::handle("set_parameter", juce::var(req.get()), processor, identity);
    const auto parsed = juce::JSON::parse(response);
    REQUIRE_FALSE(parsed.isVoid());

    REQUIRE(static_cast<bool>(parsed.getProperty("success", false)));

    const auto flush = parsed.getProperty("flush", {});
    REQUIRE_FALSE(flush.isVoid());
    REQUIRE(static_cast<bool>(flush.getProperty("plugin_unavailable", false)));

    processor.releaseResources();
}

