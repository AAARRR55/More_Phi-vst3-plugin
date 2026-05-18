#include "NeuralMasteringDatasetSchema.h"

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

bool allFinite(const MasteringTargetVector& values) noexcept
{
    return allFinite(values.eq)
        && allFinite(values.dynamics)
        && allFinite(values.stereo)
        && allFinite(values.harmonic)
        && allFinite(values.limiter)
        && allFinite(values.loudness);
}

bool allFinite(const NeuralMasteringFeatureFrame& frame) noexcept
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

bool hasValidFeatureFrame(const NeuralMasteringFeatureFrame& frame) noexcept
{
    return frame.schemaVersion == kNeuralMasteringFeatureSchemaVersion
        && frame.sampleRate > 0.0
        && (frame.channelCount == 1 || frame.channelCount == 2)
        && frame.blockSize > 0
        && allFinite(frame);
}

bool sameNonzeroFingerprint(const NeuralMasteringDatasetItem& a,
                            const NeuralMasteringDatasetItem& b) noexcept
{
    return a.sourceFingerprint != 0 && a.sourceFingerprint == b.sourceFingerprint;
}

} // namespace

NeuralMasteringDatasetValidationResult
validateNeuralMasteringDatasetItem(const NeuralMasteringDatasetItem& item) noexcept
{
    NeuralMasteringDatasetValidationResult result;

    if (item.schemaVersion != kNeuralMasteringDatasetSchemaVersion)
        result.addIssue(NeuralMasteringDatasetValidationIssue::SchemaVersionMismatch);

    if (!item.provenanceComplete)
        result.addIssue(NeuralMasteringDatasetValidationIssue::MissingProvenance);

    if (item.licenseStatus != NeuralMasteringLicenseStatus::Approved)
        result.addIssue(NeuralMasteringDatasetValidationIssue::LicenseNotApproved);

    if (item.split == NeuralMasteringDatasetSplit::Unassigned)
        result.addIssue(NeuralMasteringDatasetValidationIssue::SplitUnassigned);

    if (item.referenceQuality != NeuralMasteringReferenceQuality::Reviewed)
        result.addIssue(NeuralMasteringDatasetValidationIssue::ReferenceQualityNotReviewed);

    if (item.unsupportedMaterial)
        result.addIssue(NeuralMasteringDatasetValidationIssue::UnsupportedMaterial);

    if (!item.hasReferenceMaster)
        result.addIssue(NeuralMasteringDatasetValidationIssue::MissingReference);

    if (item.sourceFingerprint == 0)
        result.addIssue(NeuralMasteringDatasetValidationIssue::MissingSourceFingerprint);

    if (!hasValidFeatureFrame(item.featureFrame))
        result.addIssue(NeuralMasteringDatasetValidationIssue::InvalidFeatureFrame);

    if (!allFinite(item.targetVector))
        result.addIssue(NeuralMasteringDatasetValidationIssue::InvalidTargetVector);

    return result;
}

NeuralMasteringDatasetValidationResult
validateNeuralMasteringDatasetSplitIsolation(const NeuralMasteringDatasetItem* items,
                                             std::size_t itemCount) noexcept
{
    NeuralMasteringDatasetValidationResult result;

    if (items == nullptr && itemCount > 0)
    {
        result.addIssue(NeuralMasteringDatasetValidationIssue::SplitIsolationViolation);
        return result;
    }

    for (std::size_t i = 0; i < itemCount; ++i)
    {
        for (std::size_t j = i + 1; j < itemCount; ++j)
        {
            if (items[i].split != items[j].split && sameNonzeroFingerprint(items[i], items[j]))
            {
                result.addIssue(NeuralMasteringDatasetValidationIssue::SplitIsolationViolation);
                return result;
            }
        }
    }

    return result;
}

} // namespace more_phi
