/*
 * More-Phi — Core/MultibandSplitter.h
 *
 * 4-band Linkwitz-Riley (LR4) crossover filter bank.
 *
 * Crossover frequencies (configurable, defaults 80 / 250 / 5000 Hz):
 *   Band 0: Sub   — DC … fc0
 *   Band 1: Low   — fc0 … fc1
 *   Band 2: Mid   — fc1 … fc2
 *   Band 3: High  — fc2 … Nyquist
 *
 * LR4 design: two cascaded 2nd-order Butterworth stages at the same fc.
 * Property: LP + HP sums to flat magnitude (0 dB) at all frequencies.
 * Phase at the crossover frequency is 180° (not 0°) — inherent to
 * the LR4 topology. Magnitude flatness is all that matters here
 * because bands pass through independent dynamics before recombination.
 *
 * Implementation: serial cascade — each stage's LP and HP outputs feed the
 * next crossover.  Flat summation of all 4 bands is guaranteed by the LR4
 * property applied recursively.
 *
 * Thread safety:
 *   setCrossoverFrequencies() / prepare() — message thread only.
 *   processBlock() — audio thread only, noexcept.
 */
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>

namespace more_phi {

class MultibandSplitter
{
public:
    static constexpr int   kNumBands    = 4;
    static constexpr int   kMaxChannels = 2;
    static constexpr float kDefaultCrossFreqs[3] = { 80.0f, 250.0f, 5000.0f };

    MultibandSplitter();

    // ── Configuration (message thread) ───────────────────────────────────────

    /**
     * Set the three crossover frequencies (Hz).
     * Takes effect immediately on the NEXT call to prepare() — call prepare()
     * after changing frequencies to avoid filter discontinuities.
     */
    void setCrossoverFrequencies(float f1Hz, float f2Hz, float f3Hz) noexcept;

    /**
     * Allocate per-band output buffers and design filter coefficients.
     * Must be called from prepareToPlay() — never from the audio thread.
     */
    void prepare(double sampleRate, int maxBlockSize);

    /** Reset all biquad states to silence (safe between songs / offline renders). */
    void reset() noexcept;

    // ── Audio thread ─────────────────────────────────────────────────────────

    /**
     * Split input into 4 bands and write each to bands[0..3].
     * Each output buffer must have ≥ numSamples capacity per channel.
     *
     * Bands are written to raw channel pointers provided via setBandBuffers().
     * Alternatively, use the AudioBuffer overload below.
     *
     * noexcept: pure biquad arithmetic on pre-allocated state.
     */
    void processBlock(const juce::AudioBuffer<float>& input,
                      juce::AudioBuffer<float>        bands[kNumBands]) noexcept;

    // ── Getters ───────────────────────────────────────────────────────────────
    float getCrossoverFreq(int idx) const noexcept { return crossFreqs_[idx]; }

private:
    // ── Biquad primitives ────────────────────────────────────────────────────

    struct BiquadCoeffs { float b0{}, b1{}, b2{}, a1{}, a2{}; };
    struct BiquadState  { float z1{}, z2{}; };

    float processBiquad(float x, BiquadState& st, const BiquadCoeffs& c) noexcept
    {
        const float y = c.b0 * x + st.z1;
        st.z1 = c.b1 * x - c.a1 * y + st.z2;
        st.z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    // LR4 LP: two cascaded Butterworth-2 LP stages
    float processLR4_LP(float x, int xoverIdx, int ch) noexcept
    {
        x = processBiquad(x, xo_[xoverIdx].lpS1[ch], xo_[xoverIdx].lp);
        x = processBiquad(x, xo_[xoverIdx].lpS2[ch], xo_[xoverIdx].lp);
        return x;
    }

    // LR4 HP: two cascaded Butterworth-2 HP stages
    float processLR4_HP(float x, int xoverIdx, int ch) noexcept
    {
        x = processBiquad(x, xo_[xoverIdx].hpS1[ch], xo_[xoverIdx].hp);
        x = processBiquad(x, xo_[xoverIdx].hpS2[ch], xo_[xoverIdx].hp);
        return x;
    }

    // Compute 2nd-order Butterworth LP+HP biquad coefficients
    static void computeButterworth2(float fc, double sr,
                                    BiquadCoeffs& lp,
                                    BiquadCoeffs& hp) noexcept;

    // ── Per-crossover filter state ───────────────────────────────────────────

    struct Crossover
    {
        BiquadCoeffs lp, hp;                   // shared between the two cascade stages
        BiquadState  lpS1[kMaxChannels]{};     // LP stage 1
        BiquadState  lpS2[kMaxChannels]{};     // LP stage 2
        BiquadState  hpS1[kMaxChannels]{};     // HP stage 1
        BiquadState  hpS2[kMaxChannels]{};     // HP stage 2
    };

    Crossover xo_[3]{};   // 3 crossovers for 4 bands

    double sampleRate_{ 48000.0 };
    float  crossFreqs_[3]{ 80.f, 250.f, 5000.f };
    int    maxBlockSize_{ 512 };

    void computeAllCoeffs() noexcept;
};

} // namespace more_phi
