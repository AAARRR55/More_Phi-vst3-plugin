/*
 * More-Phi — Core/AudioCaptureRing.h
 *
 * Single-producer (audio thread) / single-consumer (analysis thread) ring of
 * stereo float frames. Power-of-two capacity. Cache-line-aligned indices,
 * mirroring LockFreeQueue.h.
 *
 * Thread roles (strict):
 *   write()         — audio thread ONLY. Lock-free, no allocation, noexcept.
 *   readNewest()    — analysis thread ONLY.
 *   capturedFrames()/capacity()/reset() — analysis/message thread only.
 *
 * The producer advances a monotonic totalWritten_ counter; the consumer
 * derives how many frames are currently available (min(total, capacity) once
 * the ring has wrapped). When the producer laps the consumer, readNewest()
 * hands back the newest contiguous window rather than overlapping/corrupt
 * data — this is the property that matters when the analysis thread is slow.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace more_phi {

#if defined(_MSC_VER)
#pragma warning(push)
// Intentional cache-line alignment of indices can require padding in this type.
#pragma warning(disable: 4324)
#endif

class AudioCaptureRing
{
public:
    explicit AudioCaptureRing(std::size_t capacityFrames)
        : capacityFrames_(roundUpPow2(capacityFrames)),
          buffer_(2ull * capacityFrames_, 0.0f)
    {
    }

    void reset() noexcept
    {
        writePos_.store(0, std::memory_order_relaxed);
        totalWritten_.store(0, std::memory_order_relaxed);
    }

    // Audio thread: append `n` stereo frames. Lock-free, no allocation.
    void write(const float* left, const float* right, std::size_t n) noexcept
    {
        if (left == nullptr || right == nullptr || n == 0) return;
        const std::size_t cap = capacityFrames_;
        std::size_t w = writePos_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < n; ++i)
        {
            buffer_[0ull * cap + w] = left[i];
            buffer_[1ull * cap + w] = right[i];
            w = (w + 1) & mask_();
        }
        writePos_.store(w, std::memory_order_release);

        totalWritten_.fetch_add(n, std::memory_order_release);  // H-9: release so reader's acquire sees the write
    }

    // Analysis thread: copy the most recent `n` frames (chronological order)
    // into outL/outR. Returns the number actually copied (may be < n if fewer
    // than n frames have ever been captured).
    std::size_t readNewest(std::size_t n, float* outL, float* outR) const noexcept
    {
        if (n == 0 || outL == nullptr || outR == nullptr) return 0;
        const std::size_t cap = capacityFrames_;
        const std::uint64_t total = totalWritten_.load(std::memory_order_acquire);
        if (total == 0) return 0;
        const std::size_t available = availableFrames_(total);
        const std::size_t take = std::min(n, available);

        const std::size_t w = writePos_.load(std::memory_order_acquire);
        // The newest frame is at (w - 1) mod cap; walk back `take` frames.
        const std::size_t readStart = (w + cap - take) & mask_();
        for (std::size_t i = 0; i < take; ++i)
        {
            const std::size_t idx = (readStart + i) & mask_();
            outL[i] = buffer_[0ull * cap + idx];
            outR[i] = buffer_[1ull * cap + idx];
        }
        return take;
    }

    [[nodiscard]] std::uint64_t capturedFrames() const noexcept
    {
        return availableFrames_(totalWritten_.load(std::memory_order_acquire));
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacityFrames_; }

private:
    static std::size_t roundUpPow2(std::size_t v) noexcept
    {
        v = std::max<std::size_t>(v, 2u);
        --v;
        v |= v >> 1; v |= v >> 2; v |= v >> 4;
        v |= v >> 8; v |= v >> 16;
        if constexpr (sizeof(std::size_t) > 4) v |= v >> 32;
        return v + 1;
    }

    std::size_t mask_() const noexcept { return capacityFrames_ - 1; }

    std::size_t availableFrames_(std::uint64_t total) const noexcept
    {
        return static_cast<std::size_t>(std::min<std::uint64_t>(total, capacityFrames_));
    }

    const std::size_t capacityFrames_;
    std::vector<float> buffer_;           // [cap] lefts then [cap] rights
    alignas(64) std::atomic<std::size_t>  writePos_     { 0 };
    alignas(64) std::atomic<std::uint64_t> totalWritten_{ 0 };
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace more_phi
