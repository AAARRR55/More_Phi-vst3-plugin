/*
 * More-Phi — Core/AdaptiveEQ.h
 *
 * 32-band parametric equalizer with three-layer AI control:
 *
 *   Bands  0– 7:  LLM2Fx warm-start  (set by EQParameterTranslator)
 *   Bands  8–23:  SpectralBalance correction (±6 dB max per 1/3-oct)
 *   Bands 24–31:  ST-ITO refinement  (updated by GeneticOptimizer)
 *
 * Each band is a biquad filter (parametric peak, low/high shelf, or
 * low/high pass), with parameters delivered via std::atomic for lock-free
 * audio-thread reads.
 *
 * Thread safety:
 *   processBlock() — audio thread, noexcept.
 *   setBand() — any thread (atomic writes).
 *   prepare() — message thread only.
 */
#pragma once

#include <atomic>
#include <array>
#include <cmath>
#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

class AdaptiveEQ
{
public:
    static constexpr int kNumBands    = 32;
    static constexpr int kMaxChannels = 2;
    static constexpr float kMaxGainDB = 12.0f;

    enum class BandType : int {
        Peak      = 0,
        LowShelf  = 1,
        HighShelf = 2,
        LowPass   = 3,
        HighPass  = 4,
    };

    struct BandParams {
        float    freqHz   = 1000.f;
        float    gainDB   =    0.f;
        float    Q        =    0.707f;
        BandType type     = BandType::Peak;
        bool     enabled  = true;
    };

    AdaptiveEQ();

    // ── Configuration (any thread — atomic) ───────────────────────────────────

    void setBand(int band, const BandParams& p) noexcept;
    void setBandGain(int band, float gainDB) noexcept;
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }

    BandParams getBand(int band) const noexcept;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void prepare(double sampleRate, int maxBlockSize);
    void reset() noexcept;

    // ── Audio thread ──────────────────────────────────────────────────────────

    /** Process buf in-place through all 32 bands. noexcept. */
    void processBlock(juce::AudioBuffer<float>& buf) noexcept;

private:
    struct BiquadCoeffs {
        float b0 = 1.f, b1 = 0.f, b2 = 0.f;
        float a1 = 0.f, a2 = 0.f;
    };

    struct BiquadState {
        float z1 = 0.f, z2 = 0.f;
    };

    struct AtomicBandParams {
        std::atomic<float>    freqHz  { 1000.f };
        std::atomic<float>    gainDB  {    0.f };
        std::atomic<float>    Q       {    0.707f };
        std::atomic<int>      type    { static_cast<int>(BandType::Peak) };
        std::atomic<bool>     enabled { true };
        std::atomic<bool>     dirty   { true };   // recompute coefficients flag
    };

    std::array<AtomicBandParams, kNumBands>                      params_{};
    // coefficients: updated on audio thread when dirty flag is set
    std::array<BiquadCoeffs,     kNumBands>                      coeffs_{};
    // filter state: per-band per-channel
    std::array<std::array<BiquadState, kMaxChannels>, kNumBands> states_{};

    std::atomic<bool> enabled_ { true };
    double sampleRate_ = 48000.0;

    void recomputeCoeffs(int band) noexcept;

    static BiquadCoeffs makePeak     (float freqHz, float gainDB, float Q, double sr) noexcept;
    static BiquadCoeffs makeLowShelf (float freqHz, float gainDB, float Q, double sr) noexcept;
    static BiquadCoeffs makeHighShelf(float freqHz, float gainDB, float Q, double sr) noexcept;
    static BiquadCoeffs makeLowPass  (float freqHz, float Q,                double sr) noexcept;
    static BiquadCoeffs makeHighPass (float freqHz, float Q,                double sr) noexcept;
};

} // namespace more_phi
