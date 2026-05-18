#include "NeuralMasteringFeatureExtractor.h"

#include <cmath>

namespace more_phi {
namespace {

template <std::size_t N>
bool allFinite(const std::array<float, N>& values) noexcept
{
    for (const auto value : values)
        if (!std::isfinite(value))
            return false;

    return true;
}

} // namespace

NeuralMasteringFeatureExtractionResult
NeuralMasteringFeatureExtractor::extractFromSummary(double sampleRate,
                                                    int channelCount,
                                                    int blockSize,
                                                    std::uint64_t frameIndex,
                                                    float integratedLUFS,
                                                    float truePeakDbTp,
                                                    float spectralTilt) const noexcept
{
    NeuralMasteringFeatureExtractionResult result;
    result.frame.schemaVersion = kNeuralMasteringFeatureSchemaVersion;
    result.frame.sampleRate = sampleRate;
    result.frame.channelCount = channelCount;
    result.frame.blockSize = blockSize;
    result.frame.frameIndex = frameIndex;

    if (channelCount != 1 && channelCount != 2)
    {
        result.status = NeuralMasteringFeatureExtractionStatus::UnsupportedLayout;
        result.frame.sourceQualityScore = 0.0f;
        return result;
    }

    if (sampleRate <= 0.0 || blockSize <= 0
        || !std::isfinite(integratedLUFS)
        || !std::isfinite(truePeakDbTp)
        || !std::isfinite(spectralTilt))
    {
        result.status = NeuralMasteringFeatureExtractionStatus::InvalidInput;
        result.frame.sourceQualityScore = 0.0f;
        return result;
    }

    result.frame.integratedLUFS = integratedLUFS;
    result.frame.shortTermLUFS = integratedLUFS;
    result.frame.momentaryLUFS = integratedLUFS;
    result.frame.loudnessRange = 0.0f;
    result.frame.truePeakDbTp = truePeakDbTp;
    result.frame.crestFactorDb = 0.0f;
    result.frame.spectralTilt = spectralTilt;
    result.frame.monoFoldDownDeltaDb = 0.0f;
    result.frame.transientDensity = 0.0f;
    result.frame.harmonicRisk = 0.0f;
    result.frame.sourceQualityScore = 1.0f;

    result.status = NeuralMasteringFeatureExtractionStatus::Success;
    return result;
}

bool NeuralMasteringFeatureExtractor::isFrameFinite(const NeuralMasteringFeatureFrame& frame) noexcept
{
    return std::isfinite(frame.sampleRate)
        && std::isfinite(frame.integratedLUFS)
        && std::isfinite(frame.shortTermLUFS)
        && std::isfinite(frame.momentaryLUFS)
        && std::isfinite(frame.loudnessRange)
        && std::isfinite(frame.truePeakDbTp)
        && std::isfinite(frame.crestFactorDb)
        && std::isfinite(frame.spectralTilt)
        && std::isfinite(frame.monoFoldDownDeltaDb)
        && std::isfinite(frame.transientDensity)
        && std::isfinite(frame.harmonicRisk)
        && std::isfinite(frame.sourceQualityScore)
        && allFinite(frame.spectralBands)
        && allFinite(frame.stereoCorrelation)
        && allFinite(frame.midSideRatio);
}

} // namespace more_phi
