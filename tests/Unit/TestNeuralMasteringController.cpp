#include <catch2/catch_test_macros.hpp>

#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"
#include "AI/NeuralMasteringController.h"
#include "Core/AutoMasteringEngine.h"
#include "Plugin/PluginProcessor.h"

namespace {

more_phi::NeuralMasteringFeatureFrame validFeatureFrame()
{
    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto extracted = extractor.extractFromSummary(48000.0, 2, 512, 1000, -14.0f, -1.0f, 1.5f);
    REQUIRE(extracted.succeeded());
    return extracted.frame;
}

more_phi::NeuralMasteringRuntimeState runtimeState()
{
    more_phi::NeuralMasteringRuntimeState runtime;
    runtime.currentFrame = 1000;
    runtime.sampleRate = 48000.0;
    runtime.channelCount = 2;
    runtime.layout = more_phi::NeuralMasteringLayout::Stereo;
    return runtime;
}

} // namespace

TEST_CASE("NeuralMasteringController uses deterministic no-model baseline", "[NeuralMasteringController][US2]")
{
    more_phi::NeuralMasteringController controller;
    controller.setModelRunner(nullptr);

    const auto status = controller.processFeatureFrame(validFeatureFrame(), runtimeState(), false);

    CHECK(status.featureFrameValid);
    CHECK(status.plannerInvoked);
    CHECK_FALSE(status.modelInvoked);
    CHECK(status.validationAccepted);
    CHECK_FALSE(status.applicationAttempted);
    CHECK(status.lastValidation.plan.valid);
}

TEST_CASE("NeuralMasteringController applies validated plans outside the audio callback", "[NeuralMasteringController][US3]")
{
    more_phi::AutoMasteringEngine engine;
    engine.prepare(48000.0, 512, false);

    more_phi::NeuralMasteringController controller;
    controller.setApplicationEngine(&engine);

    const auto status = controller.processFeatureFrame(validFeatureFrame(), runtimeState(), true);

    CHECK(status.featureFrameValid);
    CHECK(status.validationAccepted);
    CHECK(status.applicationAttempted);
    CHECK(status.applicationAccepted);
    CHECK(engine.hasLastSafeNeuralMasteringPlan());
}

TEST_CASE("MorePhiProcessor processBlock does not invoke neural mastering planner or model", "[NeuralMasteringController][US3][processor]")
{
    more_phi::MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 512);

    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer midi;
    buffer.clear();

    processor.processBlock(buffer, midi);

    const auto& status = processor.getNeuralMasteringController().getLastStatus();
    CHECK_FALSE(status.plannerInvoked);
    CHECK_FALSE(status.modelInvoked);
    CHECK_FALSE(status.applicationAttempted);
    CHECK_FALSE(processor.hasLastSafeNeuralMasteringPlan());

    processor.releaseResources();
}
