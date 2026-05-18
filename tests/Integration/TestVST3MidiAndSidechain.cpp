#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "MIDI/MIDIRouter.h"
#include "Plugin/PluginProcessor.h"

#include <vector>

namespace more_phi::test {

namespace {

struct MidiCapture
{
    std::vector<int> slots;
    float morphValue = -1.0f;
    int morphCalls = 0;
};

void captureSlot(int slot, void* context)
{
    static_cast<MidiCapture*>(context)->slots.push_back(slot);
}

void captureMorph(float value, void* context)
{
    auto& capture = *static_cast<MidiCapture*>(context);
    capture.morphValue = value;
    ++capture.morphCalls;
}

juce::AudioBuffer<float> makeSidechainBuffer(float amplitude, int numSamples = 512)
{
    juce::AudioBuffer<float> buffer(1, numSamples);
    for (int sample = 0; sample < numSamples; ++sample)
        buffer.setSample(0, sample, amplitude);
    return buffer;
}

juce::AudioBuffer<float> makeProcessorBuffer(int numSamples = 256)
{
    juce::AudioBuffer<float> buffer(2, numSamples);
    buffer.clear();
    return buffer;
}

juce::RangedAudioParameter& requireParameter(MorePhiProcessor& processor, const char* parameterId)
{
    auto* parameter = processor.getAPVTS().getParameter(parameterId);
    INFO("parameterId=" << parameterId);
    REQUIRE(parameter != nullptr);
    return *parameter;
}

void setNormalizedWithGesture(juce::RangedAudioParameter& parameter, float value)
{
    parameter.beginChangeGesture();
    parameter.setValueNotifyingHost(value);
    parameter.endChangeGesture();
}

} // namespace

TEST_CASE("VST3 MIDI handling maps documented C3-B3 notes to snapshot slots", "[integration][vst3][midi]")
{
    MIDIRouter router;
    MidiCapture capture;
    router.setSnapshotCallback(&captureSlot, &capture);

    juce::MidiBuffer input;
    juce::MidiBuffer filtered;

    for (int note = 48; note < 60; ++note)
        input.addEvent(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)), note - 48);

    router.processMidi(input, filtered);

    REQUIRE(capture.slots.size() == 12);
    for (int slot = 0; slot < 12; ++slot)
        REQUIRE(capture.slots[static_cast<size_t>(slot)] == slot);

    REQUIRE(filtered.isEmpty());
}

TEST_CASE("VST3 MIDI handling consumes trigger note-offs and passes out-of-range notes", "[integration][vst3][midi]")
{
    MIDIRouter router;
    MidiCapture capture;
    router.setSnapshotCallback(&captureSlot, &capture);

    juce::MidiBuffer input;
    juce::MidiBuffer filtered;
    input.addEvent(juce::MidiMessage::noteOn(1, 47, static_cast<juce::uint8>(100)), 0);
    input.addEvent(juce::MidiMessage::noteOn(1, 48, static_cast<juce::uint8>(100)), 1);
    input.addEvent(juce::MidiMessage::noteOff(1, 48), 2);
    input.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)), 3);

    router.processMidi(input, filtered);

    REQUIRE(capture.slots.size() == 1);
    REQUIRE(capture.slots[0] == 0);

    int filteredEvents = 0;
    for (const auto metadata : filtered)
    {
        const auto message = metadata.getMessage();
        REQUIRE(message.getNoteNumber() != 48);
        ++filteredEvents;
    }

    REQUIRE(filteredEvents == 2);
}

TEST_CASE("VST3 MIDI handling maps CC1 to morph fader value", "[integration][vst3][midi]")
{
    MIDIRouter router;
    MidiCapture capture;
    router.setMorphCallback(&captureMorph, &capture);

    juce::MidiBuffer input;
    juce::MidiBuffer filtered;
    input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 64), 0);

    router.processMidi(input, filtered);

    REQUIRE(capture.morphCalls == 1);
    REQUIRE(capture.morphValue == Catch::Approx(64.0f / 127.0f).margin(0.0001f));
    REQUIRE_FALSE(filtered.isEmpty());
}

TEST_CASE("VST3 MIDI handling updates processor fader path from CC1", "[integration][vst3][midi]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 100), 0);
    auto buffer = makeProcessorBuffer();

    processor.processBlock(buffer, midi);

    REQUIRE(processor.getFaderPos() == Catch::Approx(100.0f / 127.0f).margin(0.0001f));
    REQUIRE(processor.getMorphSource() == 1);

    processor.releaseResources();
}

TEST_CASE("VST3 sidechain bus and APVTS state are available to the processor", "[integration][vst3][sidechain]")
{
    MorePhiProcessor processor;
    processor.prepareToPlay(48000.0, 256);

    REQUIRE(processor.getBus(true, 1) != nullptr);
    REQUIRE(processor.getAPVTS().getParameter("sidechainEnabled") != nullptr);
    REQUIRE(processor.getAPVTS().getParameter("sidechainThreshold") != nullptr);

    setNormalizedWithGesture(requireParameter(processor, "sidechainEnabled"), 1.0f);
    setNormalizedWithGesture(requireParameter(processor, "sidechainThreshold"), 0.75f);

    auto buffer = makeProcessorBuffer();
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);

    REQUIRE(processor.getSidechainEnabled());
    REQUIRE(processor.getSidechainThreshold() == Catch::Approx(-15.0f).margin(0.001f));

    processor.releaseResources();
}

TEST_CASE("VST3 sidechain trigger opens on rising threshold edge and advances slots", "[integration][vst3][sidechain]")
{
    MIDIRouter router;
    MidiCapture capture;
    router.setSnapshotCallback(&captureSlot, &capture);
    router.setSidechainEnabled(true);
    router.setSidechainThreshold(0.1f);

    auto loud = makeSidechainBuffer(0.5f);
    router.processSidechain(loud);
    router.processSidechain(loud);

    REQUIRE(capture.slots.size() == 1);
    REQUIRE(capture.slots[0] == 0);

    auto quiet = makeSidechainBuffer(0.0f);
    router.processSidechain(quiet);
    router.processSidechain(quiet);
    router.processSidechain(quiet);

    router.processSidechain(loud);

    REQUIRE(capture.slots.size() == 2);
    REQUIRE(capture.slots[1] == 1);
}

} // namespace more_phi::test
