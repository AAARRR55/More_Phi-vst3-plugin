#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace more_phi {

class RealtimeSpectrumAnalyzer
{
public:
    static constexpr int kMaxBins = 256;

    struct SpectrumSnapshot
    {
        std::array<float, kMaxBins> magnitudeDB {};
        int binCount = 0;
        float spectralCentroid = 0.0f;
        float spectralRolloff = 0.0f;
        float spectralFlux = 0.0f;
        float crestFactor = 0.0f;
        // AUDIT-FIX (A7): these two fields are RESERVED — processFrame() never
        // assigns them, so they are always 0.0f. The plumbing (MeterWindowAccumulator,
        // MCP telemetry, SonicMasterAnalysisEngine) and TestMeterWindowAccumulator
        // depend on the fields existing, so they stay; implement the computation
        // (THD = sqrt(Σ_{h=2..5} |bin(fund*h)|²)/|bin(fund)|; program crest = a
        // slow window of per-frame crestFactor) only when a consumer needs real values.
        float thdPercent = 0.0f;
        float crestFactorProgram = 0.0f;
        float spectralTilt = 0.0f;
        uint64_t frameIndex = 0;
        double sampleRate = 0.0;
        int fftSize = 0;
    };

    RealtimeSpectrumAnalyzer();
    ~RealtimeSpectrumAnalyzer();

    RealtimeSpectrumAnalyzer(const RealtimeSpectrumAnalyzer&) = delete;
    RealtimeSpectrumAnalyzer& operator=(const RealtimeSpectrumAnalyzer&) = delete;

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;
    void processBlock(const juce::AudioBuffer<float>& buffer) noexcept;

    [[nodiscard]] bool getSnapshot(SpectrumSnapshot& out) const noexcept;

private:
    static constexpr int kDefaultFFTSize = 2048;
    static constexpr int kDefaultHopSize = 512;
    static constexpr int kMaxReadRetries = 8;

    void processFrame() noexcept;
    void publishSnapshot(const SpectrumSnapshot& snapshot) noexcept;
    void computeHannWindow() noexcept;

    double sampleRate_ = 48000.0;
    int fftSize_ = kDefaultFFTSize;
    int hopSize_ = kDefaultHopSize;
    int numBins_ = 0;
    int writePos_ = 0;
    int hopCounter_ = 0;
    uint64_t frameIndex_ = 0;

    std::unique_ptr<juce::dsp::FFT> fft_;
    std::vector<float> window_;
    // AUDIT-FIX (A3): precomputed window gains for amplitude/energy-correct
    // magnitudes. Hann coherent gain = Σw/N = 0.5; energy gain = Σw²/N = 0.375.
    // Dividing raw |X(k)| by (N * windowCoherentGain_) yields the true tone
    // amplitude (currently the /N-only division biases absolute dB by ~-6 dB).
    float windowCoherentGain_ = 1.0f;
    float windowEnergyGain_   = 1.0f;
    std::vector<float> inputBuffer_;
    std::vector<float> linearFrame_;
    // AUDIT-FIX (M2): un-windowed copy of the current frame. crestFactor /
    // peak / RMS must be computed on the raw signal, not the Hann-windowed
    // linearFrame_ (the window biases peak down by up to −6 dB and skews RMS).
    std::vector<float> rawFrame_;
    std::vector<float> fftScratch_;
    std::vector<float> rawMagnitude_;
    std::vector<float> previousMagnitudeDb_;

    mutable std::atomic<uint32_t> version_ { 0 };
    SpectrumSnapshot publishedSnapshot_;
};

} // namespace more_phi
