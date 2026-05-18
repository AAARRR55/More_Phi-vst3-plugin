#pragma once

#include "Core/NeuralMasteringTypes.h"

namespace more_phi {

enum class NeuralMasteringFeatureExtractionStatus : std::uint8_t
{
    Success,
    InvalidInput,
    UnsupportedLayout
};

struct NeuralMasteringFeatureExtractionResult
{
    NeuralMasteringFeatureExtractionStatus status = NeuralMasteringFeatureExtractionStatus::InvalidInput;
    NeuralMasteringFeatureFrame frame {};

    [[nodiscard]] bool succeeded() const noexcept { return status == NeuralMasteringFeatureExtractionStatus::Success; }
};

class NeuralMasteringFeatureExtractor
{
public:
    [[nodiscard]] NeuralMasteringFeatureExtractionResult extractFromSummary(double sampleRate,
                                                                            int channelCount,
                                                                            int blockSize,
                                                                            std::uint64_t frameIndex,
                                                                            float integratedLUFS,
                                                                            float truePeakDbTp,
                                                                            float spectralTilt = 0.0f) const noexcept;

    [[nodiscard]] static bool isFrameFinite(const NeuralMasteringFeatureFrame& frame) noexcept;
};

} // namespace more_phi
