/*
 * More-Phi — tests/Unit/TestMIDIRouting.cpp
 *
 * Runtime MIDI routing tests for MIDIRouter:
 *   - Note C3-B3 triggers snapshot slots 0-11
 *   - Out-of-range notes are passed through unchanged
 *   - CC1 drives the morph fader callback
 *   - prepare() / dropped-event counter behavior
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "MIDI/MIDIRouter.h"
#include "Core/SnapshotBank.h"

#include <juce_audio_basics/juce_audio_basics.h>

using Catch::Approx;
using namespace more_phi;

namespace {

struct MidiRecorder
{
    int lastSlot = -1;
    float lastMorph = -1.0f;

    static void snapshotCb(int slot, void* ctx)
    {
        static_cast<MidiRecorder*>(ctx)->lastSlot = slot;
    }

    static void morphCb(float value, void* ctx)
    {
        static_cast<MidiRecorder*>(ctx)->lastMorph = value;
    }
};

} // namespace

TEST_CASE("MIDIRouter maps C3-B3 notes to snapshot slots 0-11", "[midi][routing]")
{
    MIDIRouter router;
    router.prepare(16);

    MidiRecorder recorder;
    router.setSnapshotCallback(MidiRecorder::snapshotCb, &recorder);

    juce::MidiBuffer output;

    for (int note = 48; note <= 59; ++note)
    {
        INFO("Note " << note);
        recorder.lastSlot = -1;

        juce::MidiBuffer input;
        input.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), 0);
        router.processMidi(input, output);

        REQUIRE(recorder.lastSlot == note - 48);
    }
}

TEST_CASE("MIDIRouter passes out-of-range notes through unchanged", "[midi][routing]")
{
    MIDIRouter router;
    router.prepare(16);

    MidiRecorder recorder;
    router.setSnapshotCallback(MidiRecorder::snapshotCb, &recorder);

    juce::MidiBuffer output;
    juce::MidiBuffer input;
    input.addEvent(juce::MidiMessage::noteOn(1, 40, 1.0f), 0);   // below C3
    input.addEvent(juce::MidiMessage::noteOn(1, 72, 1.0f), 4);   // above B3

    router.processMidi(input, output);

    REQUIRE(recorder.lastSlot == -1);  // no snapshot triggered

    int passedThrough = 0;
    for (const auto& metadata : output)
    {
        auto msg = metadata.getMessage();
        if (msg.isNoteOn())
            ++passedThrough;
    }
    REQUIRE(passedThrough == 2);
}

TEST_CASE("MIDIRouter maps CC1 to morph callback", "[midi][routing]")
{
    MIDIRouter router;
    router.prepare(16);

    MidiRecorder recorder;
    router.setMorphCallback(MidiRecorder::morphCb, &recorder);

    juce::MidiBuffer output;
    juce::MidiBuffer input;
    input.addEvent(juce::MidiMessage::controllerEvent(1, 1, 64), 0);

    router.processMidi(input, output);

    REQUIRE(recorder.lastMorph == Approx(64.0f / 127.0f).margin(0.001f));
}

TEST_CASE("MIDIRouter dropped-event counter stays zero under normal load", "[midi][routing]")
{
    MIDIRouter router;
    router.prepare(16);
    router.resetDroppedEventCount();

    juce::MidiBuffer output;
    juce::MidiBuffer input;
    for (int i = 0; i < 16; ++i)
        input.addEvent(juce::MidiMessage::noteOn(1, 48 + (i % 12), 1.0f), i);

    router.processMidi(input, output);

    REQUIRE(router.getDroppedEventCount() == 0);
}
