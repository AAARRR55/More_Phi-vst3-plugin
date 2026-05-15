#include <catch2/catch_test_macros.hpp>

#include "Host/PluginHostManager.h"

#include <juce_audio_processors/juce_audio_processors.h>

using namespace more_phi;

TEST_CASE("PluginHostManager exclusive state guard clears when no plugin is loaded", "[host][state_guard]")
{
    PluginHostManager host;
    REQUIRE_FALSE(host.hasPlugin());

    REQUIRE(host.beginExclusivePluginUse(1) == nullptr);
    REQUIRE_FALSE(host.isExclusivePluginUseRequested());

    juce::AudioBuffer<float> buffer(2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    REQUIRE_NOTHROW(host.processBlock(buffer, midi));
    REQUIRE_FALSE(host.isExclusivePluginUseRequested());
}
