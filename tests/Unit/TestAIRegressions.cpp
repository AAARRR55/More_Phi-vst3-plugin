/*
 * More-Phi — AI/Concurrency Regression Tests
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/MCPToolsExtended.h"
#include "AI/MCPToolHandler.h"
#include "AI/MCPEQTool.h"
#include "AI/TokenOptimizer.h"
#include "Core/ParameterClassifier.h"
#include "Core/VAEMorphEngine.h"
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

TEST_CASE("MCP EQ preview and reject route through assistant preview state", "[unit][ai][mcp][eq]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(44100.0, 64);
    auto& assistant = processor.getAIAssistant();

    ParamChange change;
    change.index = 0;
    change.name = "Gain";
    change.currentValue = 0.25f;
    change.newValue = 0.75f;
    assistant.stagePendingChanges({change});

    const auto preview = MCPEQTool::previewEQ({}, processor, assistant);
    REQUIRE(preview.success);
    REQUIRE(assistant.isPreviewActive());

    const auto reject = MCPEQTool::rejectEQ({}, processor);
    REQUIRE(reject.success);
    REQUIRE_FALSE(assistant.isPreviewActive());
    REQUIRE(assistant.getPendingChanges().empty());

    processor.releaseResources();
}

TEST_CASE("MCP EQ preview reports command queue saturation", "[unit][ai][mcp][eq]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(44100.0, 64);
    auto& assistant = processor.getAIAssistant();

    while (processor.enqueueParameterSet(0, 0.5f,
                                         MorePhiProcessor::ParameterEditSource::MCP,
                                         true))
    {
    }

    ParamChange change;
    change.index = 0;
    change.name = "Gain";
    change.currentValue = 0.25f;
    change.newValue = 0.75f;
    assistant.stagePendingChanges({change});

    const auto preview = MCPEQTool::previewEQ({}, processor, assistant);
    REQUIRE_FALSE(preview.success);
    REQUIRE(preview.message.containsIgnoreCase("queue"));

    processor.releaseResources();
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

TEST_CASE("VAE stub loadModel is non-crashing and reports stub backend", "[unit][ai][vae]")
{
    VAEMorphEngine engine;
    const auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morephi_vae_stub", ".onnx");
    REQUIRE(tempFile.replaceWithText("stub"));

    const bool loaded = engine.loadModel(tempFile);
    REQUIRE(loaded);
    REQUIRE(engine.isModelLoaded());
    REQUIRE(engine.getBackendMode() == VAEMorphEngine::BackendMode::Stub);
    REQUIRE(engine.getBackendStatus().containsIgnoreCase("stub"));

    const auto latent = engine.encode({0.1f, 0.5f, 0.8f});
    REQUIRE(latent.size() == static_cast<size_t>(engine.getLatentDimensions()));

    tempFile.deleteFile();
}
