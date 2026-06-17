#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <memory>
#include <stack>
#include <mutex>

namespace more_phi {

/**
 * @brief Memory pool for reusing JUCE AudioBuffer instances to eliminate allocation overhead.
 *
 * Provides thread-safe acquisition and release of AudioBuffer<float> instances.
 * Buffers are cleared before being returned to ensure clean state.
 *
 * NOT audio-thread safe — use only from message/background threads.
 */
class AudioBufferPool
{
public:
    using AudioBufferPtr = std::unique_ptr<juce::AudioBuffer<float>>;

    /**
     * @brief Construct AudioBufferPool with specified buffer configuration.
     *
     * @param numChannels Number of channels for all buffers in the pool
     * @param numSamples Number of samples per channel for all buffers
     * @param sampleRate Sample rate for the buffers (stored for reference)
     */
    AudioBufferPool(int numChannels, int numSamples, double sampleRate);

    /**
     * @brief Acquire a buffer from the pool or create a new one if none available.
     *
     * The returned buffer is guaranteed to be cleared (filled with zeros).
     *
     * @return AudioBufferPtr Unique pointer to an AudioBuffer<float>
     */
    AudioBufferPtr acquireBuffer();

    /**
     * @brief Return a buffer to the pool for reuse.
     *
     * The buffer will be cleared and made available for subsequent acquireBuffer() calls.
     *
     * @param buffer Buffer to return to the pool
     */
    void releaseBuffer(AudioBufferPtr buffer);

    /**
     * @brief Pre-allocate the specified number of buffers in the pool.
     *
     * @param count Number of buffers to pre-create
     */
    void preallocate(size_t count);

    /**
     * @brief Get the number of buffers currently available in the pool.
     *
     * @return size_t Number of available buffers
     */
    size_t getAvailableCount() const;

private:
    /**
     * @brief Create a new buffer with the pool's configuration.
     *
     * @return AudioBufferPtr New buffer instance
     */
    AudioBufferPtr createBuffer() const;

    const int numChannels_;
    const int numSamples_;
    const double sampleRate_;

    mutable std::mutex mutex_;
    std::stack<AudioBufferPtr> availableBuffers_;
};

} // namespace more_phi