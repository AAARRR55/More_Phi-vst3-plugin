#pragma once

#include "Core/NeuralMasteringTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace more_phi {

inline constexpr std::uint32_t kNeuralMasteringModelMetadataSchemaVersion = 1;
inline constexpr std::size_t kNeuralMasteringModelIdCapacity = 64;
inline constexpr std::size_t kNeuralMasteringChecksumCapacity = 96;
inline constexpr std::size_t kNeuralMasteringSupportedSampleRateCapacity = 8;
inline constexpr std::size_t kNeuralMasteringSupportedLayoutCapacity = 4;
inline constexpr std::size_t kNeuralMasteringModelMetadataIssueCapacity = 8;

enum class NeuralMasteringLicenseStatus : std::uint8_t
{
    Approved,
    Restricted,
    Unresolved,
    Rejected
};

enum class NeuralMasteringModelMetadataIssue : std::uint8_t
{
    None,
    EmptyModelId,
    SchemaVersionMismatch,
    FeatureSchemaMismatch,
    OutputSchemaMismatch,
    AudioCallbackInferenceEnabled,
    MissingSampleRateSupport,
    MissingLayoutSupport
};

struct NeuralMasteringModelMetadata
{
    std::array<char, kNeuralMasteringModelIdCapacity> modelId {};
    std::uint32_t schemaVersion = kNeuralMasteringModelMetadataSchemaVersion;
    std::uint32_t featureSchemaVersion = kNeuralMasteringFeatureSchemaVersion;
    std::uint32_t outputSchemaVersion = kNeuralMasteringPlanSchemaVersion;
    NeuralMasteringEvidenceLevel evidenceLevel = NeuralMasteringEvidenceLevel::Planning;
    NeuralMasteringLicenseStatus licenseStatus = NeuralMasteringLicenseStatus::Unresolved;
    bool enabled = false;
    bool audioCallbackInference = false;
    std::array<double, kNeuralMasteringSupportedSampleRateCapacity> supportedSampleRates {};
    std::size_t supportedSampleRateCount = 0;
    std::array<NeuralMasteringLayout, kNeuralMasteringSupportedLayoutCapacity> supportedLayouts {};
    std::size_t supportedLayoutCount = 0;
    std::array<char, kNeuralMasteringChecksumCapacity> checksum {};

    void setModelId(std::string_view value) noexcept
    {
        modelId.fill('\0');
        const auto count = value.size() < modelId.size() ? value.size() : modelId.size() - 1;
        std::memcpy(modelId.data(), value.data(), count);
    }

    void setChecksum(std::string_view value) noexcept
    {
        checksum.fill('\0');
        const auto count = value.size() < checksum.size() ? value.size() : checksum.size() - 1;
        std::memcpy(checksum.data(), value.data(), count);
    }

    [[nodiscard]] bool hasModelId() const noexcept
    {
        return modelId[0] != '\0';
    }

    void addSupportedSampleRate(double sampleRate) noexcept
    {
        if (sampleRate <= 0.0 || supportedSampleRateCount >= supportedSampleRates.size())
            return;

        supportedSampleRates[supportedSampleRateCount++] = sampleRate;
    }

    void addSupportedLayout(NeuralMasteringLayout layout) noexcept
    {
        if (supportedLayoutCount >= supportedLayouts.size())
            return;

        supportedLayouts[supportedLayoutCount++] = layout;
    }

    [[nodiscard]] bool supportsSampleRate(double sampleRate) const noexcept
    {
        for (std::size_t i = 0; i < supportedSampleRateCount; ++i)
            if (supportedSampleRates[i] == sampleRate)
                return true;

        return false;
    }

    [[nodiscard]] bool supportsLayout(NeuralMasteringLayout layout) const noexcept
    {
        for (std::size_t i = 0; i < supportedLayoutCount; ++i)
            if (supportedLayouts[i] == layout)
                return true;

        return false;
    }
};

struct NeuralMasteringModelMetadataValidationResult
{
    std::array<NeuralMasteringModelMetadataIssue, kNeuralMasteringModelMetadataIssueCapacity> issues {};
    std::size_t issueCount = 0;
    bool valid = true;

    void addIssue(NeuralMasteringModelMetadataIssue issue) noexcept
    {
        valid = false;
        if (issueCount < issues.size())
            issues[issueCount++] = issue;
    }

    [[nodiscard]] bool hasIssue(NeuralMasteringModelMetadataIssue issue) const noexcept
    {
        for (std::size_t i = 0; i < issueCount; ++i)
            if (issues[i] == issue)
                return true;

        return false;
    }
};

[[nodiscard]] inline NeuralMasteringModelMetadataValidationResult
validateNeuralMasteringModelMetadata(const NeuralMasteringModelMetadata& metadata) noexcept
{
    NeuralMasteringModelMetadataValidationResult result;

    if (!metadata.hasModelId())
        result.addIssue(NeuralMasteringModelMetadataIssue::EmptyModelId);

    if (metadata.schemaVersion != kNeuralMasteringModelMetadataSchemaVersion)
        result.addIssue(NeuralMasteringModelMetadataIssue::SchemaVersionMismatch);

    if (metadata.featureSchemaVersion != kNeuralMasteringFeatureSchemaVersion)
        result.addIssue(NeuralMasteringModelMetadataIssue::FeatureSchemaMismatch);

    if (metadata.outputSchemaVersion != kNeuralMasteringPlanSchemaVersion)
        result.addIssue(NeuralMasteringModelMetadataIssue::OutputSchemaMismatch);

    if (metadata.audioCallbackInference)
        result.addIssue(NeuralMasteringModelMetadataIssue::AudioCallbackInferenceEnabled);

    if (metadata.supportedSampleRateCount == 0)
        result.addIssue(NeuralMasteringModelMetadataIssue::MissingSampleRateSupport);

    if (metadata.supportedLayoutCount == 0)
        result.addIssue(NeuralMasteringModelMetadataIssue::MissingLayoutSupport);

    return result;
}

} // namespace more_phi
