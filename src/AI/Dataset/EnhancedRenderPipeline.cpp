/*
 * MorphSnap — AI/Dataset/EnhancedRenderPipeline.cpp
 * Implementation of the enhanced rendering pipeline for synthetic dataset generation.
 */
#include "EnhancedRenderPipeline.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>
#include <numeric>

namespace morphsnap {

EnhancedRenderPipeline::EnhancedRenderPipeline()
{
    // Register all supported audio formats
    formatManager_.registerBasicFormats();

    // Register FLAC format (available in JUCE)
    formatManager_.registerFormat(new juce::FlacAudioFormat(), true);
}

RenderResult EnhancedRenderPipeline::render(int sampleIndex,
                                            const juce::AudioBuffer<float>& sourceAudio,
                                            const RenderConfig& config,
                                            std::function<void(juce::AudioBuffer<float>&)> processCallback)
{
    RenderResult result;
    result.sampleIndex = sampleIndex;

    auto startTime = juce::Time::getCurrentTime();

    try
    {
        // Create a copy of the source audio for processing
        juce::AudioBuffer<float> workingBuffer(sourceAudio.getNumChannels(),
                                                sourceAudio.getNumSamples());

        // Copy source to working buffer
        for (int channel = 0; channel < sourceAudio.getNumChannels(); ++channel)
        {
            workingBuffer.copyFrom(channel, 0, sourceAudio, channel, 0, sourceAudio.getNumSamples());
        }

        // Apply processing callback if provided
        if (processCallback)
        {
            processCallback(workingBuffer);
        }

        // Validate audio if enabled
        if (config.validateOutput)
        {
            if (!validateAudio(workingBuffer, config, result.peakDb, result.rmsDb))
            {
                result.hasSilence = result.rmsDb < config.silenceThresholdDb;
                result.hasClipping = result.peakDb > config.clippingThresholdDb;
            }
        }
        else
        {
            result.peakDb = computePeakDb(workingBuffer);
            result.rmsDb = computeRmsDb(workingBuffer);
        }

        // Generate output filename
        result.outputFile = generateOutputFile(sampleIndex, config);

        // Ensure output directory exists
        if (!config.outputDirectory.exists())
        {
            config.outputDirectory.createDirectory();
        }

        // Write the audio file
        if (writeAudioFile(result.outputFile, workingBuffer, config.sampleRate, config.format))
        {
            result.success = true;
        }
        else
        {
            result.errorMessage = "Failed to write audio file: " + result.outputFile.getFullPathName();
        }
    }
    catch (const std::exception& e)
    {
        result.errorMessage = "Exception during render: " + juce::String(e.what());
        result.success = false;
    }
    catch (...)
    {
        result.errorMessage = "Unknown exception during render";
        result.success = false;
    }

    result.renderTimeMs = static_cast<double>(juce::Time::getCurrentTime().toMilliseconds() -
                                               startTime.toMilliseconds());

    // Update average render time for throughput estimation
    {
        const juce::ScopedLock sl(lock_);
        averageRenderTimeMs_ = (averageRenderTimeMs_ * renderCount_ + result.renderTimeMs) /
                               (renderCount_ + 1);
        ++renderCount_;
    }

    return result;
}

void EnhancedRenderPipeline::renderBatch(const juce::Array<juce::AudioBuffer<float>>& sources,
                                         const RenderConfig& config,
                                         std::function<RenderResult(int, juce::AudioBuffer<float>&)> processCallback,
                                         std::function<void(const BatchProgress&)> progressCallback)
{
    cancelFlag_ = false;
    auto startTime = juce::Time::getCurrentTime().toMilliseconds();
    BatchProgress progress;
    progress.total = sources.size();

    for (int i = 0; i < sources.size(); ++i)
    {
        if (cancelFlag_.load())
        {
            break;
        }

        // Create a working copy
        juce::AudioBuffer<float> workingBuffer(sources[i].getNumChannels(),
                                                sources[i].getNumSamples());
        for (int ch = 0; ch < sources[i].getNumChannels(); ++ch)
        {
            workingBuffer.copyFrom(ch, 0, sources[i], ch, 0, sources[i].getNumSamples());
        }

        // Call the processing callback
        RenderResult result;
        if (processCallback)
        {
            result = processCallback(i, workingBuffer);
        }
        else
        {
            // Default behavior: just render the buffer
            result = render(i, workingBuffer, config);
        }

        // Update progress
        ++progress.completed;
        if (result.success)
            ++progress.successfulRenders;
        else
            ++progress.failedRenders;

        progress.percentage = static_cast<float>(progress.completed) / progress.total * 100.0f;
        progress.currentStatus = "Rendered sample " + juce::String(i + 1) + " of " + juce::String(sources.size());

        auto currentTime = juce::Time::getCurrentTime().toMilliseconds();
        progress.elapsedMs = static_cast<double>(currentTime - startTime);

        if (progress.completed > 1)
        {
            double avgTimePerSample = progress.elapsedMs / progress.completed;
            progress.estimatedRemainingMs = avgTimePerSample * (progress.total - progress.completed);
        }

        if (progressCallback)
        {
            progressCallback(progress);
        }

        if (onProgress)
        {
            onProgress(progress.percentage / 100.0f, progress.currentStatus);
        }
    }
}

void EnhancedRenderPipeline::renderBatchParallel(const juce::Array<juce::AudioBuffer<float>>& sources,
                                                 const RenderConfig& config,
                                                 int numThreads,
                                                 std::function<RenderResult(int, juce::AudioBuffer<float>&)> processCallback,
                                                 std::function<void(const BatchProgress&)> progressCallback)
{
    cancelFlag_ = false;
    auto startTime = juce::Time::getCurrentTime().toMilliseconds();

    // Ensure output directory exists before parallel processing
    if (!config.outputDirectory.exists())
    {
        config.outputDirectory.createDirectory();
    }

    // Thread-safe progress tracking
    std::atomic<int> completed{0};
    std::atomic<int> successful{0};
    std::atomic<int> failed{0};
    std::atomic<int> nextIndex{0};

    const int total = sources.size();
    const int actualThreads = std::min(numThreads, static_cast<int>(std::thread::hardware_concurrency()));

    // Create thread pool
    std::vector<std::thread> threads;
    threads.reserve(actualThreads);

    auto workerFunction = [&]()
    {
        while (!cancelFlag_.load())
        {
            int index = nextIndex.fetch_add(1);
            if (index >= total)
                break;

            // Create a working copy of the source
            juce::AudioBuffer<float> workingBuffer(sources[index].getNumChannels(),
                                                    sources[index].getNumSamples());
            for (int ch = 0; ch < sources[index].getNumChannels(); ++ch)
            {
                workingBuffer.copyFrom(ch, 0, sources[index], ch, 0, sources[index].getNumSamples());
            }

            // Process and render
            RenderResult result;
            if (processCallback)
            {
                result = processCallback(index, workingBuffer);
            }
            else
            {
                result = render(index, workingBuffer, config);
            }

            // Update atomic counters
            ++completed;
            if (result.success)
                ++successful;
            else
                ++failed;
        }
    };

    // Launch worker threads
    for (int t = 0; t < actualThreads; ++t)
    {
        threads.emplace_back(workerFunction);
    }

    // Progress monitoring thread
    bool progressThreadRunning = true;
    std::thread progressThread([&]()
    {
        while (progressThreadRunning && !cancelFlag_.load())
        {
            BatchProgress progress;
            progress.completed = completed.load();
            progress.total = total;
            progress.successfulRenders = successful.load();
            progress.failedRenders = failed.load();
            progress.percentage = static_cast<float>(progress.completed) / progress.total * 100.0f;
            progress.currentStatus = "Parallel rendering: " + juce::String(progress.completed) +
                                     "/" + juce::String(progress.total);

            auto currentTime = juce::Time::getCurrentTime().toMilliseconds();
            progress.elapsedMs = static_cast<double>(currentTime - startTime);

            if (progress.completed > 0)
            {
                double avgTimePerSample = progress.elapsedMs / progress.completed;
                progress.estimatedRemainingMs = avgTimePerSample * (progress.total - progress.completed);
            }

            if (progressCallback)
            {
                progressCallback(progress);
            }

            if (onProgress)
            {
                onProgress(progress.percentage / 100.0f, progress.currentStatus);
            }

            // Check if we're done
            if (progress.completed >= progress.total)
                break;

            // Sleep to avoid excessive progress updates
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    // Wait for all worker threads to complete
    for (auto& thread : threads)
    {
        if (thread.joinable())
            thread.join();
    }

    progressThreadRunning = false;
    if (progressThread.joinable())
        progressThread.join();

    // Final progress update
    BatchProgress finalProgress;
    finalProgress.completed = completed.load();
    finalProgress.total = total;
    finalProgress.successfulRenders = successful.load();
    finalProgress.failedRenders = failed.load();
    finalProgress.percentage = 100.0f;
    finalProgress.currentStatus = "Batch rendering complete";
    finalProgress.elapsedMs = static_cast<double>(juce::Time::getCurrentTime().toMilliseconds() - startTime);
    finalProgress.estimatedRemainingMs = 0.0;

    if (progressCallback)
    {
        progressCallback(finalProgress);
    }

    if (onProgress)
    {
        onProgress(1.0f, "Batch rendering complete");
    }
}

juce::AudioBuffer<float> EnhancedRenderPipeline::extractSegment(const juce::AudioBuffer<float>& source,
                                                                RenderSegment segment,
                                                                const RenderConfig& config)
{
    float durationSeconds = config.getSegmentDuration(segment);
    int segmentSamples = static_cast<int>(durationSeconds * config.sampleRate);
    int sourceSamples = source.getNumSamples();
    int numChannels = source.getNumChannels();

    juce::AudioBuffer<float> segmentBuffer(numChannels, segmentSamples);

    if (sourceSamples <= 0 || numChannels <= 0)
    {
        segmentBuffer.clear();
        return segmentBuffer;
    }

    // Determine start position based on segment type
    int startPosition = 0;

    switch (segment)
    {
        case RenderSegment::Transient:
            // Transient segment starts from the beginning
            startPosition = 0;
            break;

        case RenderSegment::SteadyState:
            // Steady-state segment starts after the transient (skip first 10% or 2 seconds)
            startPosition = std::min(static_cast<int>(sourceSamples * 0.1),
                                     static_cast<int>(2.0 * config.sampleRate));
            break;

        case RenderSegment::Full:
        case RenderSegment::Custom:
        default:
            startPosition = 0;
            break;
    }

    // Ensure we don't exceed source bounds
    int availableSamples = std::min(segmentSamples, sourceSamples - startPosition);

    if (availableSamples <= 0)
    {
        segmentBuffer.clear();
        return segmentBuffer;
    }

    // Copy the segment
    segmentBuffer.clear();
    for (int ch = 0; ch < numChannels; ++ch)
    {
        segmentBuffer.copyFrom(ch, 0, source, ch, startPosition, availableSamples);
    }

    return segmentBuffer;
}

bool EnhancedRenderPipeline::validateAudio(const juce::AudioBuffer<float>& buffer,
                                           const RenderConfig& config,
                                           float& outPeakDb,
                                           float& outRmsDb)
{
    outPeakDb = computePeakDb(buffer);
    outRmsDb = computeRmsDb(buffer);

    // Check for silence (RMS below threshold)
    bool hasSilence = outRmsDb < config.silenceThresholdDb;

    // Check for clipping (peak above threshold)
    bool hasClipping = outPeakDb > config.clippingThresholdDb;

    return !hasSilence && !hasClipping;
}

bool EnhancedRenderPipeline::writeAudioFile(const juce::File& file,
                                            const juce::AudioBuffer<float>& buffer,
                                            double sampleRate,
                                            OutputFormat format)
{
    // Ensure parent directory exists
    auto parentDir = file.getParentDirectory();
    if (!parentDir.exists())
    {
        parentDir.createDirectory();
    }

    // Delete existing file if present
    if (file.exists())
    {
        file.deleteFile();
    }

    // Create output stream
    auto outputStream = std::make_unique<juce::FileOutputStream>(file);
    if (outputStream == nullptr || !outputStream->openedOk())
    {
        return false;
    }

    // Create format writer
    auto writer = createWriter(std::move(outputStream), sampleRate, buffer.getNumChannels(), format);
    if (writer == nullptr)
    {
        return false;
    }

    // Write the audio data
    bool success = writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples());
    writer->flush();

    return success;
}

juce::File EnhancedRenderPipeline::generateOutputFile(int sampleIndex,
                                                      const RenderConfig& config,
                                                      RenderSegment segment)
{
    juce::String segmentSuffix;

    switch (segment)
    {
        case RenderSegment::Transient:
            segmentSuffix = "_transient";
            break;
        case RenderSegment::SteadyState:
            segmentSuffix = "_steady";
            break;
        case RenderSegment::Full:
            segmentSuffix = "_full";
            break;
        case RenderSegment::Custom:
            segmentSuffix = "_custom";
            break;
    }

    juce::String filename = config.filePrefix +
                            juce::String("_") +
                            juce::String(sampleIndex).paddedLeft('0', 6) +
                            segmentSuffix +
                            config.getFileExtension();

    return config.outputDirectory.getChildFile(filename);
}

bool EnhancedRenderPipeline::isFormatSupported(OutputFormat format) const
{
    switch (format)
    {
        case OutputFormat::WAV32Float:
        case OutputFormat::WAV24:
            // WAV is always supported via basic formats
            return true;

        case OutputFormat::FLAC24:
            // Check if FLAC format is available
            return formatManager_.findFormatForFileExtension("flac") != nullptr;

        default:
            return false;
    }
}

double EnhancedRenderPipeline::estimateBatchRenderTime(int numSamples, const RenderConfig& config) const
{
    // Use average render time if available, otherwise estimate based on target throughput
    double avgRenderTime = averageRenderTimeMs_;

    if (avgRenderTime <= 0.0)
    {
        // Estimate based on target throughput: 10,000 samples/hour
        // = 10,000 samples / 3,600,000 ms = 360 ms per sample
        avgRenderTime = 360.0;  // ms per sample
    }

    return avgRenderTime * numSamples;
}

float EnhancedRenderPipeline::computePeakDb(const juce::AudioBuffer<float>& buffer)
{
    float peak = 0.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* channelData = buffer.getReadPointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            float absValue = std::abs(channelData[sample]);
            if (absValue > peak)
            {
                peak = absValue;
            }
        }
    }

    return linearToDb(peak);
}

float EnhancedRenderPipeline::computeRmsDb(const juce::AudioBuffer<float>& buffer)
{
    if (buffer.getNumSamples() == 0 || buffer.getNumChannels() == 0)
    {
        return -100.0f;  // Return minimum dB for empty buffer
    }

    double sumSquares = 0.0;
    int totalSamples = buffer.getNumSamples() * buffer.getNumChannels();

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        const float* channelData = buffer.getReadPointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            float sampleValue = channelData[sample];
            sumSquares += static_cast<double>(sampleValue) * sampleValue;
        }
    }

    double rms = std::sqrt(sumSquares / totalSamples);
    return linearToDb(static_cast<float>(rms));
}

float EnhancedRenderPipeline::linearToDb(float linear)
{
    // Clamp to avoid log(0)
    constexpr float minLinear = 1e-10f;
    if (linear < minLinear)
    {
        return -100.0f;  // Minimum dB
    }

    return 20.0f * std::log10(linear);
}

std::unique_ptr<juce::AudioFormatWriter> EnhancedRenderPipeline::createWriter(
    std::unique_ptr<juce::OutputStream> outputStream,
    double sampleRate,
    int numChannels,
    OutputFormat format)
{
    switch (format)
    {
        case OutputFormat::WAV32Float:
        {
            juce::WavAudioFormat wavFormat;
            return std::unique_ptr<juce::AudioFormatWriter>(
                wavFormat.createWriterFor(outputStream.release(),
                                          sampleRate,
                                          static_cast<unsigned int>(numChannels),
                                          32,  // 32-bit float
                                          {},
                                          0));
        }

        case OutputFormat::WAV24:
        {
            juce::WavAudioFormat wavFormat;
            return std::unique_ptr<juce::AudioFormatWriter>(
                wavFormat.createWriterFor(outputStream.release(),
                                          sampleRate,
                                          static_cast<unsigned int>(numChannels),
                                          24,  // 24-bit integer
                                          {},
                                          0));
        }

        case OutputFormat::FLAC24:
        {
            juce::FlacAudioFormat flacFormat;
            return std::unique_ptr<juce::AudioFormatWriter>(
                flacFormat.createWriterFor(outputStream.release(),
                                           sampleRate,
                                           static_cast<unsigned int>(numChannels),
                                           24,  // 24-bit
                                           {},
                                           0));
        }

        default:
            return nullptr;
    }
}

void EnhancedRenderPipeline::updateProgress(BatchProgress& progress,
                                            int completed,
                                            int total,
                                            const juce::String& status,
                                            juce::int64 startTime)
{
    progress.completed = completed;
    progress.total = total;
    progress.percentage = total > 0 ? static_cast<float>(completed) / total * 100.0f : 0.0f;
    progress.currentStatus = status;

    auto currentTime = juce::Time::getCurrentTime().toMilliseconds();
    progress.elapsedMs = static_cast<double>(currentTime - startTime);

    if (completed > 1)
    {
        double avgTimePerSample = progress.elapsedMs / completed;
        progress.estimatedRemainingMs = avgTimePerSample * (total - completed);
    }
}

} // namespace morphsnap
