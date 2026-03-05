/*
 * MorphSnap — AI/Dataset/DatasetGenerator.h
 * Generates datasets by rendering audio with various plugin parameter states.
 * Connects MorphSnap's hosting capabilities with a batch rendering engine.
 */
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include "../../Host/IPluginHostManager.h"
#include "../../Core/ParameterState.h"
#include "../../Core/MorphProcessor.h"
#include <vector>
#include <functional>

namespace morphsnap {

struct GenerationConfig
{
    juce::File outputDirectory;
    int samplesPerState = 100;     // Number of random parameter states to generate
    double sampleRate = 44100.0;
    int blockSize = 512;
    float renderDurationSeconds = 2.0f; // Seconds of audio to render per state
    bool randomizeBinaryParams = true;
    bool respectsSanityConfig = true;
    SanityConfig sanityConfig;
    juce::StringArray dangerousParamSubstrings = { "Volume", "Gain", "Bypass", "Dry/Wet", "Mix", "Pitch" };
};

class DatasetGenerator
{
public:
    DatasetGenerator(IPluginHostManager& hostManager);
    ~DatasetGenerator();

    /** Starts the batch generation process. 
     *  @param inputSource An optional source audio file to process through the plugin.
     *                     If null, will use an internal impulse or white noise for character analysis.
     */
    bool generate(const GenerationConfig& config, const juce::File& inputSource = {});

    /** Callback for progress updates (0.0 to 1.0) */
    std::function<void(float, juce::String)> onProgress;

private:
    bool renderSingleState(int index, 
                          const GenerationConfig& config, 
                          const std::vector<float>& parameters,
                          const juce::File& inputFile,
                          juce::AudioFormatWriter* writer,
                          juce::AudioBuffer<float>& outBuffer);

    std::vector<float> generateRandomParameters(int count, const GenerationConfig& config);
    
    IPluginHostManager& hostManager_;
    juce::AudioFormatManager formatManager_;
};

} // namespace morphsnap
