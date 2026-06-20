#pragma once

#include "Core/NeuralMasteringTypes.h"

#include <cstdint>

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

/**
 * Aggregated analysis tap consumed by the full-frame extractor.
 *
 * These are exactly the values the AutoMasteringEngine already computes on the
 * audio thread for its meters (LUFSMeter, RealtimeSpectrumAnalyzer,
 * StereoFieldAnalyzer, EnvelopeFollower). The extractor itself is called on
 * the message thread from a low-rate timer; the caller snapshots these
 * scalars/arrays off the analysers before invoking extractFromAnalysis().
 *
 * All pointers may be null; null arrays are zero-filled in the resulting frame
 * (the model treats missing bands as "no information" rather than failing).
 */
struct NeuralMasteringAnalysisSnapshot
{
    float integratedLUFS = 0.0f;
    float shortTermLUFS  = 0.0f;
    float momentaryLUFS  = 0.0f;
    float loudnessRange  = 0.0f;
    float truePeakDbTp   = 0.0f;
    float crestFactorDb  = 0.0f;
    float spectralTilt   = 0.0f;
    float monoFoldDownDeltaDb = 0.0f;
    float transientDensity = 0.0f;
    float harmonicRisk    = 0.0f;
    float sourceQualityScore = 1.0f;

    // Down-sampled spectral content (must point to >= kNeuralMasteringSpectralBandCount floats).
    const float* spectralBands = nullptr;
    // Per-band stereo correlation in [-1, 1] (>= kNeuralMasteringStereoBandCount floats).
    const float* stereoCorrelation = nullptr;
    // Per-band mid/side energy ratio (>= kNeuralMasteringStereoBandCount floats).
    const float* midSideRatio = nullptr;
};

class NeuralMasteringFeatureExtractor
{
public:
    /**
     * Original summary extractor — populates only integratedLUFS, shortTerm/
     * momentary LUFS (mirrors of integrated), truePeakDbTp, and spectralTilt;
     * all other frame fields are left zeroed. Behaviour is UNCHANGED from
     * v3.3.0; existing callers and tests rely on this exact behaviour.
     */
    [[nodiscard]] NeuralMasteringFeatureExtractionResult extractFromSummary(double sampleRate,
                                                                            int channelCount,
                                                                            int blockSize,
                                                                            std::uint64_t frameIndex,
                                                                            float integratedLUFS,
                                                                            float truePeakDbTp,
                                                                            float spectralTilt = 0.0f) const noexcept;

    /**
     * Full-frame extractor for the neural mastering model input head.
     *
     * Fills every field of NeuralMasteringFeatureFrame from a live analysis
     * snapshot. Non-finite scalars are coerced to 0.0f so a single bad meter
     * reading cannot poison the model input. Null band arrays are zero-filled
     * (the frame remains finite — the model treats zeros as "no band data").
     *
     * Returns UnsupportedLayout for channel counts other than 1/2, and
     * InvalidInput for non-positive sample rate / block size.
     */
    [[nodiscard]] NeuralMasteringFeatureExtractionResult
    extractFromAnalysis(double sampleRate,
                        int channelCount,
                        int blockSize,
                        std::uint64_t frameIndex,
                        const NeuralMasteringAnalysisSnapshot& snapshot) const noexcept;

    [[nodiscard]] static bool isFrameFinite(const NeuralMasteringFeatureFrame& frame) noexcept;
};

} // namespace more_phi
