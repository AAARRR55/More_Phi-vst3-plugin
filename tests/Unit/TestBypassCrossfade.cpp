// tests/Unit/TestBypassCrossfade.cpp
//
// C-6 FIX (audit): pins the bypass wet/dry crossfade. Previously toggling
// bypass hard-switched the hosted plugin in/out in one block → click. The fix
// ramps bypassMix_ (1.0 = wet, 0.0 = dry) over kBypassRampBlocks and crossfades
// wet↔dry. This test asserts the mix ramps (not snaps) on both engage/release.
//
// Note: this exercises MorePhiProcessor without a hosted plugin. With no host
// plugin loaded, the wet path is a passthrough, so "wet" and "dry" are the same
// audio — but the bypassMix_ state machine (the thing we're pinning) is
// independent of the hosted plugin and ramps identically. We assert on the
// public getBypassMix() diagnostic, not on audio samples.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Plugin/PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

using namespace more_phi;
using Catch::Approx;

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlockSize   = 256;

// Run one processBlock of silence through the processor.
void runBlock(MorePhiProcessor& p)
{
    juce::AudioBuffer<float> buffer(2, kBlockSize);
    buffer.clear();
    juce::MidiBuffer midi;
    p.processBlock(buffer, midi);
}

void setBypass(MorePhiProcessor& p, bool on)
{
    auto& param = p.getAPVTS().getParameter("bypass");
    REQUIRE(param != nullptr);
    param.beginChangeGesture();
    param.setValueNotifyingHost(on ? 1.0f : 0.0f);
    param.endChangeGesture();
}

} // namespace

TEST_CASE("Bypass crossfade ramps mix down on engage and up on release",
          "[bypass][c6]")
{
    MorePhiProcessor p;
    p.prepareToPlay(kSampleRate, kBlockSize);

    // Run a few blocks so bypassMixInitialized_ sets and the mix settles at
    // its first-block target. With bypass OFF (default), the target is 1.0.
    for (int i = 0; i < 8; ++i)
        runBlock(p);
    REQUIRE(p.getBypassMix() == Approx(1.0f).margin(1e-3f));

    // Engage bypass. The mix must NOT snap to 0 in one block — it ramps down.
    setBypass(p, true);
    runBlock(p);
    {
        const float m = p.getBypassMix();
        INFO("after 1 bypass block, mix = " << m);
        REQUIRE(m < 1.0f);     // it moved toward dry
        REQUIRE(m > 0.0f);     // but did not snap to fully dry
    }

    // After enough blocks it reaches fully dry (mix ≈ 0).
    for (int i = 0; i < MorePhiProcessor::kBypassRampBlocks + 2; ++i)
        runBlock(p);
    REQUIRE(p.getBypassMix() == Approx(0.0f).margin(1e-3f));

    // Release bypass. The mix must ramp UP (not snap to 1).
    setBypass(p, false);
    runBlock(p);
    {
        const float m = p.getBypassMix();
        INFO("after 1 release block, mix = " << m);
        REQUIRE(m > 0.0f);     // it moved toward wet
        REQUIRE(m < 1.0f);     // but did not snap to fully wet
    }

    // And it settles back to fully wet.
    for (int i = 0; i < MorePhiProcessor::kBypassRampBlocks + 2; ++i)
        runBlock(p);
    REQUIRE(p.getBypassMix() == Approx(1.0f).margin(1e-3f));

    p.releaseResources();
}
