#include "DatasetGenerator.h"
#include "FeatureExtractor.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iomanip>

namespace morphsnap {

DatasetGenerator::DatasetGenerator(IPluginHostManager& hostManager)
    : hostManager_(hostManager)
{
    formatManager_.registerBasicFormats();
}

DatasetGenerator::~DatasetGenerator() {}

bool DatasetGenerator::generate(const GenerationConfig& config, const juce::File& inputSource)
{
    if (!hostManager_.hasPlugin())
        return false;

    auto* plugin = hostManager_.getPlugin();
    int paramCount = plugin->getParameters().size();

    if (!config.outputDirectory.exists())
        config.outputDirectory.createDirectory();

    juce::File audioFile = config.outputDirectory.getChildFile("dataset_audio.wav");
    juce::File metadataFile = config.outputDirectory.getChildFile("dataset_metadata.json");

    if (audioFile.exists()) audioFile.deleteFile();

    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::AudioFormatWriter> writer(wavFormat.createWriterFor(
        new juce::FileOutputStream(audioFile),
        config.sampleRate,
        2, // Stereo
        24, // 24-bit
        {},
        0
    ));

    if (writer == nullptr) return false;

    nlohmann::json metadata = nlohmann::json::object();
    metadata["version"] = "1.0.0";
    metadata["timestamp"] = juce::Time::getCurrentTime().toMilliseconds();
    metadata["plugin"] = plugin->getName().toStdString();
    metadata["samples"] = nlohmann::json::array();

    hostManager_.prepare(config.sampleRate, config.blockSize, 2);
    FeatureExtractor extractor;

    for (int i = 0; i < config.samplesPerState; ++i)
    {
        if (onProgress)
            onProgress(static_cast<float>(i) / config.samplesPerState, "Generating state " + juce::String(i + 1));

        auto params = generateRandomParameters(paramCount, config);
        
        // Apply parameters to plugin
        for (int p = 0; p < paramCount; ++p)
            plugin->getParameters()[p]->setValueNotifyingHost(params[p]);

        juce::AudioBuffer<float> clipBuffer;
        if (renderSingleState(i, config, params, inputSource, writer.get(), clipBuffer))
        {
            ExtractionConfig extractConfig;
            extractConfig.sampleRate = config.sampleRate;
            auto features = extractor.extract(clipBuffer, extractConfig);

            nlohmann::json entry;
            entry["id"] = i;
            entry["parameters"] = params;
            entry["features"] = {
                {"rms", features.temporal.rmsEnergy},
                {"peak", features.temporal.peakAmplitude},
                {"spectral_centroid", features.spectral.spectralCentroid},
                {"spectral_flatness", features.spectral.spectralFlatness},
                {"mfcc", features.spectral.mfcc}
            };
            entry["timestamp"] = juce::Time::getCurrentTime().toMilliseconds();
            metadata["samples"].push_back(entry);
        }
        // --- STABILITY FIX: Prevent PC Freeze ---
        // 1. Periodically flush the writer to disk to avoid massive cache buildup
        if (i % 50 == 0) 
            writer->flush();

        // 2. Yield thread to OS to prevent UI lockout/thermal spiking
        juce::Thread::sleep(1); 
        // ----------------------------------------
    }

    // Save metadata
    std::ofstream o(metadataFile.getFullPathName().toStdString());
    o << metadata.dump(4);
    
    return true;
}

bool DatasetGenerator::renderSingleState(int index, 
                                        const GenerationConfig& config, 
                                        const std::vector<float>& parameters,
                                        const juce::File& inputFile,
                                        juce::AudioFormatWriter* writer,
                                        juce::AudioBuffer<float>& outBuffer)
{
    int numSamples = static_cast<int>(config.renderDurationSeconds * config.sampleRate);
    outBuffer.setSize(2, numSamples);
    outBuffer.clear();
    
    juce::AudioBuffer<float> buffer(2, config.blockSize);
    juce::MidiBuffer midi;

    // Optional: Load input file if provided
    std::unique_ptr<juce::AudioFormatReader> reader;
    if (inputFile.existsAsFile())
        reader.reset(formatManager_.createReaderFor(inputFile));

    int samplesProcessed = 0;
    while (samplesProcessed < numSamples)
    {
        int toProcess = std::min(config.blockSize, numSamples - samplesProcessed);
        buffer.setSize(2, toProcess, false, true, true);

        if (reader != nullptr)
            reader->read(&buffer, 0, toProcess, samplesProcessed % (reader->lengthInSamples == 0 ? 1 : reader->lengthInSamples), true, true);
        else
        {
            // Generate white noise for character analysis if no input source
            juce::Random& r = juce::Random::getSystemRandom();
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            {
                auto* writePtr = buffer.getWritePointer(ch);
                for (int s = 0; s < toProcess; ++s)
                    writePtr[s] = (r.nextFloat() * 2.0f - 1.0f) * 0.1f; // -20dB noise
            }
        }

        hostManager_.processBlock(buffer, midi);
        
        // Copy to output collection buffer for features
        for(int ch = 0; ch < buffer.getNumChannels(); ++ch)
            outBuffer.copyFrom(ch, samplesProcessed, buffer, ch, 0, toProcess);

        if (!writer->writeFromAudioSampleBuffer(buffer, 0, toProcess))
            return false;

        samplesProcessed += toProcess;
    }

    return true;
}

std::vector<float> DatasetGenerator::generateRandomParameters(int count, const GenerationConfig& config)
{
    std::vector<float> params(count);
    juce::Random& r = juce::Random::getSystemRandom();

    auto* plugin = hostManager_.getPlugin();

    for (int i = 0; i < count; ++i)
    {
        if (config.respectsSanityConfig && plugin != nullptr)
        {
            auto* param = plugin->getParameters()[i];
            juce::String name = param->getName(128);
            
            bool isDangerous = false;
            for (const auto& dangerous : config.dangerousParamSubstrings)
            {
                if (name.containsIgnoreCase(dangerous))
                {
                    params[i] = param->getValue(); // Keep current value
                    isDangerous = true;
                    break;
                }
            }
            if (isDangerous) continue;

            // Also check protected indices directly from SanityConfig for GeneticEngine compatibility
            SanityConfig sc = config.sanityConfig;
            if (sc.protectedIndices.count(i) > 0)
            {
                params[i] = param->getValue();
                continue;
            }
        }

        // Discrete parameter handling: ensure we snap to valid steps if supported by plugin
        auto* param = plugin->getParameters()[i];
        if (param->getNumSteps() > 0 && param->getNumSteps() != 0x7fffffff)
        {
            int step = r.nextInt(param->getNumSteps());
            params[i] = static_cast<float>(step) / static_cast<float>(juce::jmax(1, param->getNumSteps() - 1));
        }
        else if (!config.randomizeBinaryParams && param->isBoolean())
        {
            params[i] = param->getValue();
        }
        else
        {
            params[i] = r.nextFloat();
        }
    }
    
    return params;
}

} // namespace morphsnap
