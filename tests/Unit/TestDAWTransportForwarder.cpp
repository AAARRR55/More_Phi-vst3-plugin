// More-Phi — DAWTransportForwarder unit tests (Catch2 v3)
//
// These tests define the NEW API (producer updateFromAudioThread + consumer
// getSnapshot -> std::optional<TransportState>) before the header is rewritten.
#include <juce_audio_basics/juce_audio_basics.h>
#include "../../src/Host/DAWTransportForwarder.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <atomic>
#include <thread>

namespace {
// Minimal stub AudioPlayHead returning a fixed PositionInfo, mirroring the
// pattern in tools/HeadlessHost/HeadlessHostMain.cpp.
class StubPlayHead : public juce::AudioPlayHead
{
public:
    explicit StubPlayHead(PositionInfo info) : info_(info) {}
    Optional<PositionInfo> getPosition() const override { return info_; }
private:
    PositionInfo info_;
};

juce::AudioPlayHead::PositionInfo makeInfo(double bpm, int num, int den,
                                           bool playing, bool looping,
                                           double ppq, double seconds)
{
    juce::AudioPlayHead::PositionInfo info;
    info.setBpm(juce::Optional<double>{ bpm });
    info.setTimeSignature(juce::Optional<juce::AudioPlayHead::TimeSignature>{ { num, den } });
    info.setIsPlaying(playing);
    info.setIsLooping(looping);
    info.setPpqPosition(juce::Optional<double>{ ppq });
    info.setTimeInSeconds(juce::Optional<double>{ seconds });
    return info;
}
} // namespace

TEST_CASE("DAWTransportForwarder publishes a coherent snapshot", "[host][transport]")
{
    DAWTransportForwarder forwarder;

    // Before any update: no snapshot available.
    REQUIRE_FALSE(forwarder.getSnapshot().has_value());

    StubPlayHead playHead(makeInfo(140.0, 7, 8, true, true, 12.5, 26.0));
    forwarder.updateFromAudioThread(&playHead);

    const auto snap = forwarder.getSnapshot();
    REQUIRE(snap.has_value());
    REQUIRE_THAT(snap->bpm,            Catch::Matchers::WithinAbs(140.0, 1e-6));
    REQUIRE(snap->timeSigNumerator   == 7);
    REQUIRE(snap->timeSigDenominator == 8);
    REQUIRE(snap->isPlaying == true);
    REQUIRE(snap->isLooping == true);
    REQUIRE_THAT(snap->ppqPosition,    Catch::Matchers::WithinAbs(12.5, 1e-6));
    REQUIRE_THAT(snap->timeInSeconds,  Catch::Matchers::WithinAbs(26.0, 1e-6));
    REQUIRE(snap->version > 0u);
}

TEST_CASE("DAWTransportForwarder version bumps on each publish", "[host][transport]")
{
    DAWTransportForwarder forwarder;
    StubPlayHead a(makeInfo(100.0, 4, 4, false, false, 0.0, 0.0));
    StubPlayHead b(makeInfo(110.0, 4, 4, false, false, 1.0, 1.0));

    forwarder.updateFromAudioThread(&a);
    const auto first  = forwarder.getSnapshot();
    forwarder.updateFromAudioThread(&b);
    const auto second = forwarder.getSnapshot();

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE(second->version > first->version);
    REQUIRE_THAT(second->bpm, Catch::Matchers::WithinAbs(110.0, 1e-6));
}

TEST_CASE("DAWTransportForwarder never observes a torn snapshot under a concurrent producer", "[host][transport]")
{
    // Producer alternates two internally-consistent states; the consumer must
    // never see a mix (e.g. bpm from state A with ppq from state B).
    DAWTransportForwarder forwarder;

    std::atomic<bool> stop{ false };
    std::thread producer([&] {
        bool toggle = false;
        while (!stop.load(std::memory_order_relaxed))
        {
            // State A: bpm 100, ppq 10.0.  State B: bpm 200, ppq 20.0.
            // A valid read has bpm/ppq from the SAME state.
            StubPlayHead ph(toggle
                ? makeInfo(100.0, 4, 4, true, false, 10.0, 10.0)
                : makeInfo(200.0, 4, 4, true, false, 20.0, 20.0));
            forwarder.updateFromAudioThread(&ph);
            toggle = !toggle;
        }
    });

    bool observedValid = false;
    for (int i = 0; i < 20000; ++i)
    {
        const auto snap = forwarder.getSnapshot();
        if (snap.has_value())
        {
            const bool isStateA = (snap->bpm == 100.0 && snap->ppqPosition == 10.0);
            const bool isStateB = (snap->bpm == 200.0 && snap->ppqPosition == 20.0);
            INFO("torn snapshot: bpm=" << snap->bpm << " ppq=" << snap->ppqPosition);
            REQUIRE((isStateA || isStateB));
            observedValid = true;
        }
    }
    REQUIRE(observedValid);

    stop.store(true, std::memory_order_relaxed);
    producer.join();
}
