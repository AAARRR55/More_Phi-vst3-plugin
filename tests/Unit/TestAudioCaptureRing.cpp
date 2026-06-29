// tests/Unit/TestAudioCaptureRing.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "Core/AudioCaptureRing.h"

#include <algorithm>
#include <numeric>
#include <vector>

TEST_CASE("AudioCaptureRing round-trips a single write then read", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/1024);
    float l[64], r[64];
    std::iota(std::begin(l), std::end(l), 0.0f);
    std::iota(std::begin(r), std::end(r), 100.0f);
    ring.write(l, r, 64);

    std::vector<float> lo(64, -1.0f), ro(64, -1.0f);
    REQUIRE(ring.readNewest(64, lo.data(), ro.data()) == 64);
    CHECK_THAT(lo[0],  Catch::Matchers::WithinAbs(0.0f,   1e-4f));
    CHECK_THAT(lo[63], Catch::Matchers::WithinAbs(63.0f,  1e-4f));
    CHECK_THAT(ro[0],  Catch::Matchers::WithinAbs(100.0f, 1e-4f));
}

TEST_CASE("AudioCaptureRing returns newest window when producer laps consumer", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/256);
    // Write 1000 frames of ramped data; only the newest 128 survive.
    for (int batch = 0; batch < 10; ++batch)
    {
        std::vector<float> l(100), r(100);
        std::iota(l.begin(), l.end(), static_cast<float>(batch * 100));
        std::iota(r.begin(), r.end(), static_cast<float>(batch * 100) + 0.5f);
        ring.write(l.data(), r.data(), 100);
    }

    std::vector<float> lo(128, -1.0f), ro(128, -1.0f);
    REQUIRE(ring.readNewest(128, lo.data(), ro.data()) == 128);
    // The newest frame written was the 1000th; the window's last sample is the
    // last value of the final batch (99 + 900 = 999).
    CHECK_THAT(lo[127], Catch::Matchers::WithinAbs(999.0f, 1.0f));
}

TEST_CASE("AudioCaptureRing reports captured frame count", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/512);
    CHECK(ring.capturedFrames() == 0);
    std::vector<float> l(200, 0.0f), r(200, 0.0f);
    ring.write(l.data(), r.data(), 200);
    CHECK(ring.capturedFrames() == 200);
}

TEST_CASE("AudioCaptureRing caps capturedFrames at capacity after wrap", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/512);
    std::vector<float> l(1000, 1.0f), r(1000, 1.0f);
    ring.write(l.data(), r.data(), 1000);
    // capacityFrames is rounded up to a power of two (512); capturedFrames
    // saturates at capacity once the producer has lapped it.
    CHECK(ring.capturedFrames() == 512);
}

TEST_CASE("AudioCaptureRing resets on reset()", "[SonicMaster][Ring]")
{
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/512);
    std::vector<float> l(100, 1.0f), r(100, 1.0f);
    ring.write(l.data(), r.data(), 100);
    ring.reset();
    CHECK(ring.capturedFrames() == 0);
}

TEST_CASE("AudioCaptureRing availableFrames saturates at capacity after wrap (no hasWrapped_ race)",
          "[SonicMaster][Ring]")
{
    // Regression: availableFrames_ must return exactly capacity once the
    // producer has written more than the ring can hold, regardless of how
    // the consumer's reads interleave with writes.
    more_phi::AudioCaptureRing ring(/*capacityFrames=*/512);
    std::vector<float> l(512, 1.0f), r(512, 1.0f);
    // Write exactly one ring's worth.
    ring.write(l.data(), r.data(), 256);
    CHECK(ring.capturedFrames() == 256);
    // Wrap around: write another full ring's worth.
    ring.write(l.data(), r.data(), 256);
    CHECK(ring.capturedFrames() == 512);  // saturated at capacity
    // Read the newest 256 — must be all 1.0 (from the second write).
    std::vector<float> lo(256, -1.0f), ro(256, -1.0f);
    REQUIRE(ring.readNewest(256, lo.data(), ro.data()) == 256);
    for (auto v : lo) CHECK(v == 1.0f);
    for (auto v : ro) CHECK(v == 1.0f);
}
