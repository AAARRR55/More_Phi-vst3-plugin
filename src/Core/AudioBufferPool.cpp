#include "AudioBufferPool.h"

namespace morphsnap {

AudioBufferPool::AudioBufferPool(int numChannels, int numSamples, double sampleRate)
    : numChannels_(numChannels)
    , numSamples_(numSamples)
    , sampleRate_(sampleRate)
{
}

AudioBufferPool::AudioBufferPtr AudioBufferPool::acquireBuffer()
{
    std::lock_guard<std::mutex> lock(mutex_);

    AudioBufferPtr buffer;

    if (!availableBuffers_.empty())
    {
        buffer = std::move(availableBuffers_.top());
        availableBuffers_.pop();
    }
    else
    {
        buffer = createBuffer();
    }

    // Clear the buffer to ensure clean state
    if (buffer)
    {
        buffer->clear();
    }

    return buffer;
}

void AudioBufferPool::releaseBuffer(AudioBufferPtr buffer)
{
    if (!buffer)
        return;

    std::lock_guard<std::mutex> lock(mutex_);

    // Clear buffer before returning to pool
    buffer->clear();

    availableBuffers_.push(std::move(buffer));
}

void AudioBufferPool::preallocate(size_t count)
{
    std::lock_guard<std::mutex> lock(mutex_);

    for (size_t i = 0; i < count; ++i)
    {
        auto buffer = createBuffer();
        if (buffer)
        {
            availableBuffers_.push(std::move(buffer));
        }
    }
}

size_t AudioBufferPool::getAvailableCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return availableBuffers_.size();
}

AudioBufferPool::AudioBufferPtr AudioBufferPool::createBuffer() const
{
    return std::make_unique<juce::AudioBuffer<float>>(numChannels_, numSamples_);
}

} // namespace morphsnap