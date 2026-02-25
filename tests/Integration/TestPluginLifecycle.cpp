/*
 * MorphSnap — Integration Tests
 * Tests the full MorphSnapProcessor lifecycle:
 *   - Construction / destruction
 *   - prepareToPlay / releaseResources
 *   - processBlock (empty, with MIDI, live audio)
 *   - State save/restore round-trip
 *   - APVTS parameter automation
 *
 * These tests exercise the same code paths a DAW would use,
 * validating MORPH-028 compatibility concerns programmatically.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Plugin/PluginProcessor.h"

using Catch::Approx;
using namespace morphsnap;

// ─────────────────────────────────────────────────────────────────────────────
//  Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Processor: construction and destruction do not crash", "[integration][lifecycle]")
{
    // Mimics DAW instantiation
    auto proc = std::make_unique<MorphSnapProcessor>();
    REQUIRE(proc->getName().isNotEmpty());
    REQUIRE(proc->getTotalNumInputChannels() >= 2);
    REQUIRE(proc->getTotalNumOutputChannels() >= 2);
    proc.reset();  // Destruction — must not crash
}

TEST_CASE("Processor: prepareToPlay and releaseResources cycle", "[integration][lifecycle]")
{
    MorphSnapProcessor proc;
    proc.prepareToPlay(44100.0, 512);
    proc.releaseResources();

    // Double prepare (some hosts do this)
    proc.prepareToPlay(48000.0, 256);
    proc.prepareToPlay(96000.0, 128);
    proc.releaseResources();
}

// ─────────────────────────────────────────────────────────────────────────────
//  processBlock
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Processor: processBlock with empty buffer does not crash", "[integration][process]")
{
    MorphSnapProcessor proc;
    proc.prepareToPlay(44100.0, 512);

    juce::AudioBuffer<float> buffer(2, 512);
    buffer.clear();
    juce::MidiBuffer midi;

    proc.processBlock(buffer, midi);
    proc.releaseResources();
}

TEST_CASE("Processor: processBlock with live audio preserves buffer integrity", "[integration][process]")
{
    MorphSnapProcessor proc;
    proc.prepareToPlay(48000.0, 256);

    juce::AudioBuffer<float> buffer(2, 256);
    // Fill with a sine wave
    for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < 256; ++s)
            buffer.setSample(ch, s, std::sin(static_cast<float>(s) * 0.1f) * 0.5f);

    juce::MidiBuffer midi;
    proc.processBlock(buffer, midi);

    // Buffer should still contain finite values (no NaN/inf corruption)
    for (int ch = 0; ch < 2; ++ch)
        for (int s = 0; s < 256; ++s)
            REQUIRE(std::isfinite(buffer.getSample(ch, s)));

    proc.releaseResources();
}

TEST_CASE("Processor: multiple processBlock calls simulate sustained playback", "[integration][process]")
{
    MorphSnapProcessor proc;
    proc.prepareToPlay(44100.0, 128);

    juce::AudioBuffer<float> buffer(2, 128);
    buffer.clear();
    juce::MidiBuffer midi;

    // Simulate 1 second of playback (344 blocks at 44100/128)
    for (int i = 0; i < 344; ++i)
        proc.processBlock(buffer, midi);

    proc.releaseResources();
}

// ─────────────────────────────────────────────────────────────────────────────
//  State Persistence (getStateInformation / setStateInformation)
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Processor: state save/restore round-trip preserves APVTS values", "[integration][state]")
{
    // Save state from first instance
    juce::MemoryBlock stateData;
    {
        MorphSnapProcessor proc;
        proc.prepareToPlay(44100.0, 512);

        // Set some parameter values via APVTS
        if (auto* param = proc.getAPVTS().getParameter("sanityEnabled"))
            param->setValueNotifyingHost(1.0f);  // Enable
        if (auto* param = proc.getAPVTS().getParameter("sidechainThreshold"))
            param->setValueNotifyingHost(0.5f);

        proc.getStateInformation(stateData);
        proc.releaseResources();
    }

    // Restore into a fresh instance
    {
        MorphSnapProcessor proc2;
        proc2.prepareToPlay(44100.0, 512);
        proc2.setStateInformation(stateData.getData(),
                                   static_cast<int>(stateData.getSize()));

        // Verify the restored values match
        if (auto* param = proc2.getAPVTS().getParameter("sanityEnabled"))
            REQUIRE(param->getValue() == Approx(1.0f).margin(0.01f));

        proc2.releaseResources();
    }
}

TEST_CASE("Processor: empty state data does not crash setStateInformation", "[integration][state]")
{
    MorphSnapProcessor proc;
    proc.prepareToPlay(44100.0, 512);

    // Must not crash with empty/null data
    proc.setStateInformation(nullptr, 0);

    // Must not crash with random garbage
    uint8_t garbage[64] = {};
    proc.setStateInformation(garbage, sizeof(garbage));

    proc.releaseResources();
}

// ─────────────────────────────────────────────────────────────────────────────
//  APVTS Parameters
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Processor: all new APVTS parameters exist", "[integration][params]")
{
    MorphSnapProcessor proc;

    REQUIRE(proc.getAPVTS().getParameter("sanityEnabled") != nullptr);
    REQUIRE(proc.getAPVTS().getParameter("recallMode") != nullptr);
    REQUIRE(proc.getAPVTS().getParameter("sidechainEnabled") != nullptr);
    REQUIRE(proc.getAPVTS().getParameter("sidechainThreshold") != nullptr);
}

TEST_CASE("Processor: parameter automation does not crash during processBlock", "[integration][params]")
{
    MorphSnapProcessor proc;
    proc.prepareToPlay(44100.0, 256);

    juce::AudioBuffer<float> buffer(2, 256);
    buffer.clear();
    juce::MidiBuffer midi;

    // Automate parameters while processing
    for (int i = 0; i < 100; ++i)
    {
        float t = static_cast<float>(i) / 100.0f;
        if (auto* p = proc.getAPVTS().getParameter("sidechainThreshold"))
            p->setValueNotifyingHost(t);
        proc.processBlock(buffer, midi);
    }

    proc.releaseResources();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Bus Layout
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Processor: supports stereo in/out layout", "[integration][buses]")
{
    MorphSnapProcessor proc;

    auto layout = proc.getBusesLayout();
    REQUIRE(layout.getMainInputChannelSet() == juce::AudioChannelSet::stereo());
    REQUIRE(layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}
