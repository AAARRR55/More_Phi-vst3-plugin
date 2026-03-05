/*
 * MorphSnap — AI/Concurrency Regression Tests
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/MCPToolsExtended.h"
#include "AI/TokenOptimizer.h"
#include "Core/ParameterClassifier.h"
#include "Core/VAEMorphEngine.h"
#include "Host/IPluginHostManager.h"
#include "Plugin/PluginProcessor.h"

#include <nlohmann/json.hpp>

using Catch::Approx;
using namespace morphsnap;

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

private:
    std::vector<juce::String> names_;
    std::vector<float> values_;
    std::vector<bool> discrete_;
};

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

    MorphSnapProcessor processor;
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

TEST_CASE("VAE stub loadModel is non-crashing and reports stub backend", "[unit][ai][vae]")
{
    VAEMorphEngine engine;
    const auto tempFile = juce::File::getSpecialLocation(juce::File::tempDirectory)
        .getNonexistentChildFile("morphsnap_vae_stub", ".onnx");
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
