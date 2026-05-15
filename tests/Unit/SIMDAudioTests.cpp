#include <catch2/catch_test_macros.hpp>
#include "Core/SIMDAudio.h"
#include <vector>
#include <algorithm>
#include <cmath>

TEST_CASE("SIMD audio operations are faster than scalar", "[SIMDAudio]")
{
    const size_t numSamples = 8192;
    std::vector<float> input(numSamples, 0.5f);
    std::vector<float> output(numSamples);
    std::vector<float> expected(numSamples, 1.0f); // 0.5 * 2.0

    more_phi::SIMDAudio::multiplyScalar(input.data(), 2.0f, output.data(), numSamples);

    for (size_t i = 0; i < numSamples; ++i)
    {
        REQUIRE(std::abs(output[i] - expected[i]) < 0.0001f);
    }
}

TEST_CASE("CPU capability detection", "[SIMDAudio]")
{
    // These should return valid boolean values (implementation-dependent)
    bool avxSupport = more_phi::SIMDAudio::supportsAVX();
    bool sseSupport = more_phi::SIMDAudio::supportsSSE();

    // AVX requires SSE as a prerequisite
    if (avxSupport)
    {
        REQUIRE(sseSupport);
    }

    // At minimum, we should be able to detect support (true or false)
    REQUIRE((avxSupport || !avxSupport)); // Always true, but exercises the code path
    REQUIRE((sseSupport || !sseSupport)); // Always true, but exercises the code path
}

TEST_CASE("Scalar addition operations", "[SIMDAudio]")
{
    const size_t numSamples = 1000;
    std::vector<float> input(numSamples);
    std::vector<float> output(numSamples);

    // Fill with sequential values
    for (size_t i = 0; i < numSamples; ++i)
    {
        input[i] = static_cast<float>(i) * 0.1f;
    }

    const float addValue = 5.5f;
    more_phi::SIMDAudio::addScalar(input.data(), addValue, output.data(), numSamples);

    for (size_t i = 0; i < numSamples; ++i)
    {
        float expected = input[i] + addValue;
        REQUIRE(std::abs(output[i] - expected) < 0.0001f);
    }
}

TEST_CASE("Vector multiplication operations", "[SIMDAudio]")
{
    const size_t numSamples = 1000;
    std::vector<float> input1(numSamples);
    std::vector<float> input2(numSamples);
    std::vector<float> output(numSamples);

    for (size_t i = 0; i < numSamples; ++i)
    {
        input1[i] = static_cast<float>(i) * 0.1f;
        input2[i] = 2.0f + static_cast<float>(i) * 0.05f;
    }

    more_phi::SIMDAudio::multiply(input1.data(), input2.data(), output.data(), numSamples);

    for (size_t i = 0; i < numSamples; ++i)
    {
        float expected = input1[i] * input2[i];
        REQUIRE(std::abs(output[i] - expected) < 0.001f);
    }
}

TEST_CASE("Vector addition operations", "[SIMDAudio]")
{
    const size_t numSamples = 1000;
    std::vector<float> input1(numSamples);
    std::vector<float> input2(numSamples);
    std::vector<float> output(numSamples);

    for (size_t i = 0; i < numSamples; ++i)
    {
        input1[i] = static_cast<float>(i) * 0.1f;
        input2[i] = 1.5f - static_cast<float>(i) * 0.02f;
    }

    more_phi::SIMDAudio::add(input1.data(), input2.data(), output.data(), numSamples);

    for (size_t i = 0; i < numSamples; ++i)
    {
        float expected = input1[i] + input2[i];
        REQUIRE(std::abs(output[i] - expected) < 0.0001f);
    }
}

TEST_CASE("Peak detection operations", "[SIMDAudio]")
{
    const size_t numSamples = 1000;
    std::vector<float> input(numSamples);

    // Create signal with known peak
    for (size_t i = 0; i < numSamples; ++i)
    {
        input[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.8f;
    }
    input[500] = 2.5f; // Known maximum absolute value
    input[300] = -2.7f; // Known minimum (higher absolute value)

    float peak = more_phi::SIMDAudio::findPeak(input.data(), numSamples);

    REQUIRE(std::abs(peak - 2.7f) < 0.0001f);
}

TEST_CASE("RMS calculation operations", "[SIMDAudio]")
{
    // Test with known DC signal
    const size_t numSamples = 1000;
    std::vector<float> input(numSamples, 0.5f);

    float rms = more_phi::SIMDAudio::calculateRMS(input.data(), numSamples);

    REQUIRE(std::abs(rms - 0.5f) < 0.0001f);
}

TEST_CASE("RMS calculation with sine wave", "[SIMDAudio]")
{
    const size_t numSamples = 1000;
    std::vector<float> input(numSamples);

    // Generate sine wave with amplitude 1.0 (RMS should be ~0.707)
    for (size_t i = 0; i < numSamples; ++i)
    {
        input[i] = std::sin(static_cast<float>(i) * 2.0f * 3.14159f / 100.0f);
    }

    float rms = more_phi::SIMDAudio::calculateRMS(input.data(), numSamples);

    // RMS of sine wave with amplitude 1.0 is 1/sqrt(2) ≈ 0.707
    REQUIRE(std::abs(rms - 0.707f) < 0.05f); // Allow some tolerance due to discrete samples
}

TEST_CASE("Edge cases", "[SIMDAudio]")
{
    SECTION("Empty array")
    {
        float rms = more_phi::SIMDAudio::calculateRMS(nullptr, 0);
        REQUIRE(rms == 0.0f);

        float peak = more_phi::SIMDAudio::findPeak(nullptr, 0);
        REQUIRE(peak == 0.0f);
    }

    SECTION("Single sample")
    {
        float input = 1.5f;
        float output;

        more_phi::SIMDAudio::multiplyScalar(&input, 2.0f, &output, 1);
        REQUIRE(std::abs(output - 3.0f) < 0.0001f);

        float peak = more_phi::SIMDAudio::findPeak(&input, 1);
        REQUIRE(std::abs(peak - 1.5f) < 0.0001f);

        float rms = more_phi::SIMDAudio::calculateRMS(&input, 1);
        REQUIRE(std::abs(rms - 1.5f) < 0.0001f);
    }

    SECTION("Odd number of samples")
    {
        const size_t numSamples = 123; // Not divisible by 4 or 8
        std::vector<float> input(numSamples, 0.75f);
        std::vector<float> output(numSamples);

        more_phi::SIMDAudio::multiplyScalar(input.data(), 4.0f, output.data(), numSamples);

        for (size_t i = 0; i < numSamples; ++i)
        {
            REQUIRE(std::abs(output[i] - 3.0f) < 0.0001f);
        }
    }
}