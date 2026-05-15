/*
 * More-Phi — docs/AudioEngineSpec_v2.md (rendered as a header for
 * discoverability; actual spec text is in docs/AudioEngineSpec_v2.md)
 *
 * More-Phi — Core/LatencyManager.h
 *
 * Centralizes all latency bookkeeping for MorePhiProcessor.
 * Reports the correct total latency to the DAW via setLatencySamples().
 *
 * Total latency equation:
 *   totalLatency = oversamplingLatency
 *                + fftWindowLatency          (spectral morph mode only)
 *                + hostedPluginLatency        (passthrough from hosted VST3)
 *
 * Threading:
 *   All setters are called from the message thread (prepareToPlay).
 *   getTotal() is safe to call from any thread (atomic read).
 */
#pragma once

#include <atomic>
#include <algorithm>

namespace more_phi {

class LatencyManager
{
public:
    LatencyManager() = default;

    // ─── Setters (message thread / prepareToPlay) ─────────────────────────

    /** Latency from OversamplingWrapper::getLatencyInSamples(). */
    void setOversamplingLatency(int samples) noexcept
    {
        oversamplingLatency_.store(std::max(0, samples), std::memory_order_relaxed);
        recompute();
    }

    /**
     * Latency from FFT windowing in spectral morph mode.
     * = (fftSize / 2) samples at the current sample rate.
     * Pass 0 when not using spectral processing.
     */
    void setFFTWindowLatency(int samples) noexcept
    {
        fftWindowLatency_.store(std::max(0, samples), std::memory_order_relaxed);
        recompute();
    }

    /**
     * Latency reported by the currently-loaded hosted plugin.
     * Read from AudioPluginInstance::getLatencySamples() after plugin load.
     */
    void setHostedPluginLatency(int samples) noexcept
    {
        hostedPluginLatency_.store(std::max(0, samples), std::memory_order_relaxed);
        recompute();
    }

    /**
     * Latency introduced by the mastering chain lookahead (BrickwallLimiter).
     * = 4 ms lookahead in samples at the current sample rate.
     * E.g. 192 samples @ 48 kHz.
     */
    void setMasteringChainLatency(int samples) noexcept
    {
        masteringChainLatency_.store(std::max(0, samples), std::memory_order_relaxed);
        recompute();
    }

    // ─── Getter (any thread) ──────────────────────────────────────────────

    /** Total reported latency in samples. Feed directly to setLatencySamples(). */
    [[nodiscard]] int getTotal() const noexcept
    {
        return total_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] int getOversamplingLatency()   const noexcept { return oversamplingLatency_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getFFTWindowLatency()      const noexcept { return fftWindowLatency_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getHostedPluginLatency()   const noexcept { return hostedPluginLatency_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getMasteringChainLatency() const noexcept { return masteringChainLatency_.load(std::memory_order_relaxed); }

private:
    void recompute() noexcept
    {
        int total = oversamplingLatency_.load(std::memory_order_relaxed)
                  + fftWindowLatency_.load(std::memory_order_relaxed)
                  + hostedPluginLatency_.load(std::memory_order_relaxed)
                  + masteringChainLatency_.load(std::memory_order_relaxed);
        total_.store(total, std::memory_order_relaxed);
    }

    std::atomic<int> oversamplingLatency_   { 0 };
    std::atomic<int> fftWindowLatency_      { 0 };
    std::atomic<int> hostedPluginLatency_   { 0 };
    std::atomic<int> masteringChainLatency_ { 0 };
    std::atomic<int> total_                 { 0 };
};

} // namespace more_phi
