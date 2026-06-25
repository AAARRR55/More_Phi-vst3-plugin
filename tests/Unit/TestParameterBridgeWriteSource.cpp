// tests/Unit/TestParameterBridgeWriteSource.cpp
//
// AUDIT (E2, 2026-06-25) regression guard for the per-parameter write-source
// stamp + write-precedence conflict counter in ParameterBridge.
//
// Context: the audit's original E2 premise ("two uncoordinated hosted-parameter
// write paths") turned out to be factually wrong — both hosted writers (Neural
// OzonePlanApplicator + MCP set_parameter) share ONE FIFO command queue, and the
// agent path (RealtimeControlAgent) writes More-Phi's OWN APVTS params, not the
// hosted plugin. So this stamp does NOT arbitrate a real conflict. What it does
// is make a same-parameter, different-source edit burst OBSERVABLE: when a write
// from a DIFFERENT source arrives within kWritePrecedenceSettleMs of the previous
// write to the same index, the conflict counter ticks. That is pure observability
// for debugging (e.g. a user manually tweaking a control the neural engine is
// also driving), audio-safe (fixed array + relaxed atomics, no locks).
//
// These tests exercise the stamp + counter directly (no plugin required — the
// stamp methods never touch the hosted plugin), so they run in every CI config.
#include "Host/ParameterBridge.h"
#include "Host/IPluginHostManager.h"

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <chrono>
#include <thread>

namespace {

// Minimal host with no plugin — the stamp methods don't need one. Mirrors the
// FakeHostManager shape from TestRecallRamp.cpp but with null plugin + the full
// IPluginHostManager surface stubbed (only what ParameterBridge's ctor/dtor touch).
class NullHostManager final : public more_phi::IPluginHostManager
{
public:
    void prepare(double, int, int) override {}
    void releaseResources() override {}
    bool loadPlugin(const juce::PluginDescription&) override { return false; }
    void unloadPlugin() override {}
    bool hasPlugin() const override { return false; }
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) noexcept override {}
    juce::AudioPluginInstance* getPlugin() override { return nullptr; }
    const juce::AudioPluginInstance* getPlugin() const override { return nullptr; }
    const juce::PluginDescription* getLastDescription() const override { return nullptr; }
    juce::AudioPluginFormatManager& getFormatManager() override { return fm_; }
    juce::KnownPluginList& getKnownPlugins() override { return list_; }
    void scanPluginFolders() override {}

private:
    juce::AudioPluginFormatManager fm_;
    juce::KnownPluginList list_;
};

} // namespace

TEST_CASE("ParameterBridge stamps the last write source per parameter", "[ParameterBridge][E2]")
{
    NullHostManager host;
    more_phi::ParameterBridge bridge(host);

    // Fresh bridge: every slot reports Unknown.
    REQUIRE(bridge.getLastWriteSource(7) == more_phi::HostedWriteSource::Unknown);

    bridge.noteWriteSource(7, more_phi::HostedWriteSource::Neural);
    REQUIRE(bridge.getLastWriteSource(7) == more_phi::HostedWriteSource::Neural);

    bridge.noteWriteSource(7, more_phi::HostedWriteSource::MCP);
    REQUIRE(bridge.getLastWriteSource(7) == more_phi::HostedWriteSource::MCP);

    // Other indices are unaffected.
    REQUIRE(bridge.getLastWriteSource(8) == more_phi::HostedWriteSource::Unknown);
}

TEST_CASE("ParameterBridge counts write-precedence conflicts only within the settle window",
          "[ParameterBridge][E2]")
{
    NullHostManager host;
    more_phi::ParameterBridge bridge(host);
    constexpr int kIdx = 3;

    // Same source, repeated — NOT a conflict (the neural engine re-driving its
    // own control, or a user re-tweaking their own edit).
    bridge.noteWriteSource(kIdx, more_phi::HostedWriteSource::Neural);
    bridge.noteWriteSource(kIdx, more_phi::HostedWriteSource::Neural);
    bridge.noteWriteSource(kIdx, more_phi::HostedWriteSource::Neural);
    REQUIRE(bridge.getWritePrecedenceConflicts() == 0);

    // Different source within the settle window → conflict.
    bridge.noteWriteSource(kIdx, more_phi::HostedWriteSource::MCP);
    REQUIRE(bridge.getWritePrecedenceConflicts() == 1);

    // Out-of-range index is a safe no-op (no crash, no stamp, no count).
    bridge.noteWriteSource(-1, more_phi::HostedWriteSource::MCP);
    bridge.noteWriteSource(more_phi::MAX_PARAMETERS + 5, more_phi::HostedWriteSource::MCP);
    REQUIRE(bridge.getWritePrecedenceConflicts() == 1);
    REQUIRE(bridge.getLastWriteSource(-1) == more_phi::HostedWriteSource::Unknown);

    // Reset clears the counter (not the stamps).
    bridge.resetWritePrecedenceConflicts();
    REQUIRE(bridge.getWritePrecedenceConflicts() == 0);
    REQUIRE(bridge.getLastWriteSource(kIdx) == more_phi::HostedWriteSource::MCP);
}

TEST_CASE("ParameterBridge write-precedence conflict expires after the settle window",
          "[ParameterBridge][E2]")
{
    NullHostManager host;
    more_phi::ParameterBridge bridge(host);
    constexpr int kIdx = 11;

    // First write from Neural.
    bridge.noteWriteSource(kIdx, more_phi::HostedWriteSource::Neural);

    // Sleep past kWritePrecedenceSettleMs (250 ms) so the next different-source
    // write is NOT counted as a conflict (the prior write is stale).
    std::this_thread::sleep_for(
        std::chrono::milliseconds(more_phi::ParameterBridge::kWritePrecedenceSettleMs + 60));

    const uint64_t before = bridge.getWritePrecedenceConflicts();
    bridge.noteWriteSource(kIdx, more_phi::HostedWriteSource::MCP);
    REQUIRE(bridge.getWritePrecedenceConflicts() == before);  // no new conflict

    // But a same-source write right after is still not a conflict, and a quick
    // different-source flip within the window now IS.
    bridge.noteWriteSource(kIdx, more_phi::HostedWriteSource::Neural);
    REQUIRE(bridge.getWritePrecedenceConflicts() == before + 1);
}
