#pragma once

#include "AI/NeuralMasteringModelMetadata.h"
#include "Core/NeuralMasteringTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace more_phi {

inline constexpr std::uint32_t kNeuralMasteringDatasetSchemaVersion = 1;
inline constexpr std::size_t kNeuralMasteringDatasetIdCapacity = 64;
inline constexpr std::size_t kNeuralMasteringDatasetIssueCapacity = 12;

enum class NeuralMasteringDatasetSplit : std::uint8_t
{
    Train,
    Validation,
    Test,
    Unassigned
};

enum class NeuralMasteringReferenceQuality : std::uint8_t
{
    Reviewed,
    NeedsReview,
    Rejected,
    Unsupported
};

enum class NeuralMasteringDatasetValidationIssue : std::uint8_t
{
    None,
    SchemaVersionMismatch,
    MissingProvenance,
    LicenseNotApproved,
    SplitUnassigned,
    SplitIsolationViolation,
    ReferenceQualityNotReviewed,
    UnsupportedMaterial,
    MissingReference,
    MissingSourceFingerprint,
    InvalidFeatureFrame,
    InvalidTargetVector
};

struct NeuralMasteringDatasetItem
{
    std::uint32_t schemaVersion = kNeuralMasteringDatasetSchemaVersion;
    std::array<char, kNeuralMasteringDatasetIdCapacity> itemId {};
    std::array<char, kNeuralMasteringDatasetIdCapacity> sourceId {};
    std::array<char, kNeuralMasteringDatasetIdCapacity> artistId {};
    std::array<char, kNeuralMasteringDatasetIdCapacity> sessionId {};
    NeuralMasteringDatasetSplit split = NeuralMasteringDatasetSplit::Unassigned;
    NeuralMasteringLicenseStatus licenseStatus = NeuralMasteringLicenseStatus::Unresolved;
    NeuralMasteringReferenceQuality referenceQuality = NeuralMasteringReferenceQuality::NeedsReview;
    bool provenanceComplete = false;
    bool unsupportedMaterial = false;
    bool hasReferenceMaster = false;
    std::uint64_t sourceFingerprint = 0;
    NeuralMasteringFeatureFrame featureFrame {};
    MasteringTargetVector targetVector {};

    void setItemId(std::string_view value) noexcept { copyText(itemId, value); }
    void setSourceId(std::string_view value) noexcept { copyText(sourceId, value); }
    void setArtistId(std::string_view value) noexcept { copyText(artistId, value); }
    void setSessionId(std::string_view value) noexcept { copyText(sessionId, value); }

private:
    static void copyText(std::array<char, kNeuralMasteringDatasetIdCapacity>& destination,
                         std::string_view value) noexcept
    {
        destination.fill('\0');
        const auto count = value.size() < destination.size() ? value.size() : destination.size() - 1;
        std::memcpy(destination.data(), value.data(), count);
    }
};

struct NeuralMasteringDatasetValidationResult
{
    std::array<NeuralMasteringDatasetValidationIssue, kNeuralMasteringDatasetIssueCapacity> issues {};
    std::size_t issueCount = 0;
    bool valid = true;

    void addIssue(NeuralMasteringDatasetValidationIssue issue) noexcept
    {
        valid = false;
        if (issueCount < issues.size())
            issues[issueCount++] = issue;
    }

    [[nodiscard]] bool hasIssue(NeuralMasteringDatasetValidationIssue issue) const noexcept
    {
        for (std::size_t i = 0; i < issueCount; ++i)
            if (issues[i] == issue)
                return true;

        return false;
    }
};

[[nodiscard]] NeuralMasteringDatasetValidationResult
validateNeuralMasteringDatasetItem(const NeuralMasteringDatasetItem& item) noexcept;

[[nodiscard]] NeuralMasteringDatasetValidationResult
validateNeuralMasteringDatasetSplitIsolation(const NeuralMasteringDatasetItem* items,
                                             std::size_t itemCount) noexcept;

} // namespace more_phi
