/*
 * More-Phi — AI/Dataset/EnhancedRenderPipeline.h
 * Advanced audio rendering pipeline for synthetic dataset generation.
 * Supports multi-segment rendering, multiple output formats, and parallel processing.
 */
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <functional>
#include <atomic>
#include <mutex>

namespace more_phi {

/** Segment types for multi-segment rendering */
enum class RenderSegment
{
    Full,           ///< 30 seconds - complete audio
    Transient,      ///< 2 seconds - transient analysis segment
    SteadyState,    ///< 5 seconds - steady-state analysis segment
    Custom          ///< User-defined duration
};

/** Output format options for rendered audio */
enum class OutputFormat
{
    WAV32Float,     ///< 48kHz/32-bit float WAV
    WAV24,          ///< 48kHz/24-bit WAV
    FLAC24          ///< 48kHz/24-bit FLAC
};

/** Configuration for a render operation */
struct RenderConfig
{
    double sampleRate = 48000.0;
    int blockSize = 512;
    int numChannels = 2;
    OutputFormat format = OutputFormat::WAV32Float;
    juce::File outputDirectory;
    juce::String filePrefix = "sample";

    // Segment durations in seconds
    float fullDuration = 30.0f;
    float transientDuration = 2.0f;
    float steadyStateDuration = 5.0f;
    float customDuration = 10.0f;  // Used when segment is Custom

    // Validation settings
    float silenceThresholdDb = -60.0f;
    float clippingThresholdDb = -0.1f;
    bool validateOutput = true;

    /** Get file extension for the configured format */
    juce::String getFileExtension() const
    {
        switch (format)
        {
            case OutputFormat::FLAC24: return ".flac";
            case OutputFormat::WAV24:
            case OutputFormat::WAV32Float:
            default: return ".wav";
        }
    }

    /** Get the duration for a specific segment type */
    float getSegmentDuration(RenderSegment segment) const
    {
        switch (segment)
        {
            case RenderSegment::Full: return fullDuration;
            case RenderSegment::Transient: return transientDuration;
            case RenderSegment::SteadyState: return steadyStateDuration;
            case RenderSegment::Custom: return customDuration;
            default: return fullDuration;
        }
    }
};

/** Result of a single render operation */
struct RenderResult
{
    int sampleIndex = 0;
    bool success = false;
    juce::File outputFile;
    float peakDb = 0.0f;
    float rmsDb = -100.0f;
    bool hasSilence = false;
    bool hasClipping = false;
    double renderTimeMs = 0.0;
    juce::String errorMessage;
    juce::StringArray segmentFiles;  // Files for each segment if multi-segment rendering
};

/** Progress information for batch rendering operations */
struct BatchProgress
{
    int completed = 0;
    int total = 0;
    float percentage = 0.0f;
    juce::String currentStatus;
    double elapsedMs = 0.0;
    double estimatedRemainingMs = 0.0;
    int successfulRenders = 0;
    int failedRenders = 0;
};

/**
 * Enhanced rendering pipeline for synthetic audio dataset generation.
 *
 * Features:
 * - Multi-segment rendering (full, transient, steady-state)
 * - Multiple output formats (WAV 32-bit float, WAV 24-bit, FLAC 24-bit)
 * - Batch rendering with progress callbacks
 * - Parallel rendering support
 * - Audio validation (silence and clipping detection)
 *
 * Target throughput: 10,000 samples/hour
 */
class EnhancedRenderPipeline
{
public:
    EnhancedRenderPipeline();
    ~EnhancedRenderPipeline() = default;

    // Non-copyable, movable
    EnhancedRenderPipeline(const EnhancedRenderPipeline&) = delete;
    EnhancedRenderPipeline& operator=(const EnhancedRenderPipeline&) = delete;
    EnhancedRenderPipeline(EnhancedRenderPipeline&&) = default;
    EnhancedRenderPipeline& operator=(EnhancedRenderPipeline&&) = default;

    /**
     * Render a single audio buffer to file with optional processing.
     *
     * @param sampleIndex Index for filename generation
     * @param sourceAudio Source audio buffer to render
     * @param config Render configuration
     * @param processCallback Optional callback to process audio before writing
     * @return RenderResult with success status and metadata
     */
    RenderResult render(int sampleIndex,
                        const juce::AudioBuffer<float>& sourceAudio,
                        const RenderConfig& config,
                        std::function<void(juce::AudioBuffer<float>&)> processCallback = nullptr);

    /**
     * Render multiple audio buffers in batch mode.
     *
     * @param sources Array of source audio buffers
     * @param config Render configuration
     * @param processCallback Callback to process each buffer (returns RenderResult)
     * @param progressCallback Optional callback for progress updates
     */
    void renderBatch(const juce::Array<juce::AudioBuffer<float>>& sources,
                     const RenderConfig& config,
                     std::function<RenderResult(int, juce::AudioBuffer<float>&)> processCallback,
                     std::function<void(const BatchProgress&)> progressCallback = nullptr);

    /**
     * Render multiple audio buffers in parallel using multiple threads.
     *
     * @param sources Array of source audio buffers
     * @param config Render configuration
     * @param numThreads Number of parallel render threads
     * @param processCallback Callback to process each buffer (returns RenderResult)
     * @param progressCallback Optional callback for progress updates
     */
    void renderBatchParallel(const juce::Array<juce::AudioBuffer<float>>& sources,
                             const RenderConfig& config,
                             int numThreads,
                             std::function<RenderResult(int, juce::AudioBuffer<float>&)> processCallback,
                             std::function<void(const BatchProgress&)> progressCallback = nullptr);

    /**
     * Extract a segment from source audio.
     *
     * @param source Source audio buffer
     * @param segment Segment type to extract
     * @param config Configuration with segment durations
     * @return Audio buffer containing the extracted segment
     */
    juce::AudioBuffer<float> extractSegment(const juce::AudioBuffer<float>& source,
                                            RenderSegment segment,
                                            const RenderConfig& config);

    /**
     * Validate audio buffer for silence and clipping.
     *
     * @param buffer Audio buffer to validate
     * @param config Configuration with validation thresholds
     * @param outPeakDb Output: peak level in dB
     * @param outRmsDb Output: RMS level in dB
     * @return true if audio passes validation (no silence/clipping issues)
     */
    bool validateAudio(const juce::AudioBuffer<float>& buffer,
                       const RenderConfig& config,
                       float& outPeakDb,
                       float& outRmsDb);

    /**
     * Write audio buffer to file in the specified format.
     *
     * @param file Output file
     * @param buffer Audio buffer to write
     * @param sampleRate Sample rate for the file
     * @param format Output format
     * @return true if file was written successfully
     */
    bool writeAudioFile(const juce::File& file,
                        const juce::AudioBuffer<float>& buffer,
                        double sampleRate,
                        OutputFormat format);

    /**
     * Generate output filename for a sample index.
     *
     * @param sampleIndex Sample index
     * @param config Render configuration
     * @param segment Optional segment type for multi-segment naming
     * @return Generated filename
     */
    juce::File generateOutputFile(int sampleIndex,
                                  const RenderConfig& config,
                                  RenderSegment segment = RenderSegment::Full);

    /**
     * Check if the pipeline supports a given output format.
     *
     * @param format Format to check
     * @return true if format is supported
     */
    bool isFormatSupported(OutputFormat format) const;

    /**
     * Get estimated render time for a batch.
     *
     * @param numSamples Number of samples to render
     * @param config Render configuration
     * @return Estimated time in milliseconds
     */
    double estimateBatchRenderTime(int numSamples, const RenderConfig& config) const;

    /** Progress callback for single renders */
    std::function<void(float, juce::String)> onProgress;

private:
    /**
     * Compute peak level in dB for an audio buffer.
     *
     * @param buffer Audio buffer to analyze
     * @return Peak level in dB
     */
    float computePeakDb(const juce::AudioBuffer<float>& buffer);

    /**
     * Compute RMS level in dB for an audio buffer.
     *
     * @param buffer Audio buffer to analyze
     * @return RMS level in dB
     */
    float computeRmsDb(const juce::AudioBuffer<float>& buffer);

    /**
     * Convert linear amplitude to decibels.
     *
     * @param linear Linear amplitude value
     * @return Value in decibels
     */
    float linearToDb(float linear);

    /**
     * Create an audio format writer for the specified format.
     *
     * @param outputStream Output stream to write to
     * @param sampleRate Sample rate for the file
     * @param numChannels Number of channels
     * @param format Output format
     * @return Unique pointer to format writer, or nullptr on failure
     */
    std::unique_ptr<juce::AudioFormatWriter> createWriter(
        std::unique_ptr<juce::OutputStream> outputStream,
        double sampleRate,
        int numChannels,
        OutputFormat format);

    /**
     * Update batch progress safely (thread-safe).
     *
     * @param progress Progress structure to update
     * @param completed Number of completed renders
     * @param total Total number of renders
     * @param status Current status message
     * @param startTime Start time of batch operation
     */
    void updateProgress(BatchProgress& progress,
                        int completed,
                        int total,
                        const juce::String& status,
                        juce::int64 startTime);

    juce::AudioFormatManager formatManager_;
    juce::CriticalSection lock_;
    std::atomic<bool> cancelFlag_{false};

    // Performance tracking
    static constexpr double samplesPerHourTarget_ = 10000.0;
    double averageRenderTimeMs_ = 0.0;
    int renderCount_ = 0;
};

} // namespace more_phi
