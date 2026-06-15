#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace more_phi {

class StereoFieldAnalyzer
{
public:
    static constexpr int kNumBands = 4;

    struct StereoFieldSnapshot
    {
        std::array<float, kNumBands> correlation {};
        std::array<float, kNumBands> msEnergyRatio {};
        float stereoWidth = 0.0f;
        uint64_t frameIndex = 0;
        double sampleRate = 0.0;
        int windowSamples = 0;
    };

    StereoFieldAnalyzer();
    ~StereoFieldAnalyzer();

    StereoFieldAnalyzer(const StereoFieldAnalyzer&) = delete;
    StereoFieldAnalyzer& operator=(const StereoFieldAnalyzer&) = delete;

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void processBlock(const juce::AudioBuffer<float>& buffer) noexcept;

    [[nodiscard]] bool getSnapshot(StereoFieldSnapshot& out) const noexcept;

private:
    struct BiquadCoeffs { float b0{}, b1{}, b2{}, a1{}, a2{}; };
    struct BiquadState { float z1{}, z2{}; };

    struct Crossover
    {
        BiquadCoeffs lp;
        BiquadCoeffs hp;
        std::array<BiquadState, 2> lpState {};
        std::array<BiquadState, 2> hpState {};
    };

    static constexpr int kMaxReadRetries = 8;

    static void computeButterworth2(float cutoffHz, double sampleRate, BiquadCoeffs& lp, BiquadCoeffs& hp) noexcept;
    static float processBiquad(float input, BiquadState& state, const BiquadCoeffs& coeffs) noexcept;

    void computeAllCoeffs() noexcept;
    void splitBands(float input, int streamIndex, std::array<float, kNumBands>& bands) noexcept;
    void publishCurrentWindow() noexcept;
    void publishSnapshot(const StereoFieldSnapshot& snapshot) noexcept;

    double sampleRate_ = 48000.0;
    int windowSamples_ = 4800;
    int samplesInWindow_ = 0;
    uint64_t frameIndex_ = 0;

    std::array<Crossover, 3> crossovers_ {};
    std::array<float, 3> crossoverFrequencies_ { 120.0f, 800.0f, 8000.0f };

    std::array<float, kNumBands> sumLR_ {};
    std::array<float, kNumBands> sumL2_ {};
    std::array<float, kNumBands> sumR2_ {};
    std::array<float, kNumBands> sumM2_ {};
    std::array<float, kNumBands> sumS2_ {};

    mutable std::atomic<uint32_t> version_ { 0 };
    StereoFieldSnapshot publishedSnapshot_;
};

} // namespace more_phi
