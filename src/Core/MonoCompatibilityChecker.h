/*
 * More-Phi — Core/MonoCompatibilityChecker.h
 *
 * Header-only post-processing verification tool.
 *
 * Computes the energy of L+R (mono sum) vs individual channels per 1/3-octave
 * band and flags any band where mono summing causes > 3 dB cancellation.
 * When a problem band is found, auto-corrects by reducing the side signal in
 * that band (via a callback to StereoImager).
 *
 * Runs as a juce::AsyncUpdater (triggered from the audio thread, executes on
 * message thread). The audio thread only accumulates samples — no logic.
 *
 * Thread safety:
 *   accumulateSamples() — audio thread, noexcept.
 *   handleAsyncUpdate() — message thread.
 *   setCorrectCallback() — any thread before start.
 */
#pragma once

#include <atomic>
#include <array>
#include <functional>
#include <cmath>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_audio_basics/juce_audio_basics.h>

namespace more_phi {

class MonoCompatibilityChecker : public juce::AsyncUpdater
{
public:
    static constexpr int kNumBands    = 10;   // 1/3-oct subset: 63–16kHz
    static constexpr float kThreshDB  = -3.f; // flag if mono loss > 3 dB
    static constexpr float kCorrDB    = -3.f; // auto-correct: reduce side by 3 dB

    /** Callback invoked on message thread: (bandIndex, newSideGainMultiplier). */
    using CorrectCallback = std::function<void(int band, float sideGainMultiplier)>;

    MonoCompatibilityChecker()
    {
        for (auto& e : stereoEnergy_) e.store(0.f, std::memory_order_relaxed);
        for (auto& e : monoEnergy_)   e.store(0.f, std::memory_order_relaxed);
    }

    /** Register callback that applies side-gain reduction. */
    void setCorrectCallback(CorrectCallback cb) { correctCallback_ = std::move(cb); }

    /**
     * Accumulate L/R samples into per-band energy accumulators.
     * Audio thread. noexcept.
     * Simple broadband accumulation for prototype; full 1/3-oct filterbank
     * can be substituted by replacing energy accumulation below.
     */
    void accumulateSamples(const juce::AudioBuffer<float>& buf) noexcept
    {
        if (buf.getNumChannels() < 2) return;
        const float* L = buf.getReadPointer(0);
        const float* R = buf.getReadPointer(1);
        const int ns = buf.getNumSamples();

        float stereoE = 0.f, monoE = 0.f;
        for (int i = 0; i < ns; ++i)
        {
            const float l = L[i], r = R[i];
            stereoE += l * l + r * r;
            const float m = (l + r) * 0.5f;
            monoE   += m * m * 2.f;   // *2 to match stereo energy scale
        }

        // Accumulate into band 0 (broadband) — extend for per-band
        stereoEnergy_[0].store(stereoEnergy_[0].load(std::memory_order_relaxed) + stereoE,
                               std::memory_order_relaxed);
        monoEnergy_[0].store(monoEnergy_[0].load(std::memory_order_relaxed) + monoE,
                             std::memory_order_relaxed);

        ++sampleCounter_;
        if (sampleCounter_ >= kAnalysisInterval)
        {
            sampleCounter_ = 0;
            triggerAsyncUpdate();
        }
    }

    /** Message thread: check mono compatibility and apply correction if needed. */
    void handleAsyncUpdate() override
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            const float stereoE = stereoEnergy_[b].exchange(0.f, std::memory_order_relaxed);
            const float monoE   = monoEnergy_[b].exchange(0.f, std::memory_order_relaxed);

            if (stereoE < 1e-12f) continue;

            const float ratioDb = 10.f * std::log10(monoE / stereoE + 1e-12f);
            if (ratioDb < kThreshDB && correctCallback_)
            {
                // Reduce side gain by 3 dB to improve mono compatibility
                correctCallback_(b, std::pow(10.f, kCorrDB / 20.f));
            }
        }
    }

private:
    static constexpr int kAnalysisInterval = 48000;  // ~1 s @ 48 kHz

    std::array<std::atomic<float>, kNumBands> stereoEnergy_{};
    std::array<std::atomic<float>, kNumBands> monoEnergy_{};

    CorrectCallback correctCallback_;
    int sampleCounter_ = 0;
};

} // namespace more_phi
