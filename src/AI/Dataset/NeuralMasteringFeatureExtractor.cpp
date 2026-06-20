#include "NeuralMasteringFeatureExtractor.h"

#include <algorithm>
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

float finiteOrZero(float value) noexcept
{
    return std::isfinite(value) ? value : 0.0f;
}

// Copy up to N floats from src (which may be null) into dst, coercing
// non-finite values to 0.0f. If src is null or shorter than N, the remaining
// slots are zero-filled — the frame stays finite and the model sees "no band".
template <std::size_t N>
void copyBands(std::array<float, N>& dst, const float* src) noexcept
{
    if (src == nullptr)
    {
        dst.fill(0.0f);
        return;
    }
    for (std::size_t i = 0; i < N; ++i)
        dst[i] = finiteOrZero(src[i]);
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

NeuralMasteringFeatureExtractionResult
NeuralMasteringFeatureExtractor::extractFromAnalysis(double sampleRate,
                                                     int channelCount,
                                                     int blockSize,
                                                     std::uint64_t frameIndex,
                                                     const NeuralMasteringAnalysisSnapshot& snapshot) const noexcept
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

    if (sampleRate <= 0.0 || blockSize <= 0)
    {
        result.status = NeuralMasteringFeatureExtractionStatus::InvalidInput;
        result.frame.sourceQualityScore = 0.0f;
        return result;
    }

    // Scalar features — every meter value is coerced to finite (0.0f) so a
    // single non-finite reading from the analysers cannot reach the model.
    result.frame.integratedLUFS      = finiteOrZero(snapshot.integratedLUFS);
    result.frame.shortTermLUFS       = finiteOrZero(snapshot.shortTermLUFS);
    result.frame.momentaryLUFS       = finiteOrZero(snapshot.momentaryLUFS);
    result.frame.loudnessRange       = finiteOrZero(snapshot.loudnessRange);
    result.frame.truePeakDbTp        = finiteOrZero(snapshot.truePeakDbTp);
    result.frame.crestFactorDb       = finiteOrZero(snapshot.crestFactorDb);
    result.frame.spectralTilt        = finiteOrZero(snapshot.spectralTilt);
    result.frame.monoFoldDownDeltaDb = finiteOrZero(snapshot.monoFoldDownDeltaDb);
    result.frame.transientDensity    = finiteOrZero(snapshot.transientDensity);
    result.frame.harmonicRisk        = finiteOrZero(snapshot.harmonicRisk);
    result.frame.sourceQualityScore  = finiteOrZero(snapshot.sourceQualityScore);

    // Band arrays — null/short sources are zero-filled (finite, "no band data").
    copyBands(result.frame.spectralBands, snapshot.spectralBands);
    copyBands(result.frame.stereoCorrelation, snapshot.stereoCorrelation);
    copyBands(result.frame.midSideRatio, snapshot.midSideRatio);

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
