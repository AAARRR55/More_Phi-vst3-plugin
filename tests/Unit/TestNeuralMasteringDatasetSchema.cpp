#include <catch2/catch_test_macros.hpp>

#include "AI/Dataset/NeuralMasteringDatasetSchema.h"
#include "AI/Dataset/NeuralMasteringFeatureExtractor.h"
#include "AI/NeuralMasteringModelMetadata.h"

namespace {

more_phi::NeuralMasteringDatasetItem validDatasetItem(std::uint64_t fingerprint = 1001)
{
    more_phi::NeuralMasteringFeatureExtractor extractor;
    const auto extracted = extractor.extractFromSummary(48000.0, 2, 512, 10, -14.0f, -1.0f);
    REQUIRE(extracted.succeeded());

    more_phi::NeuralMasteringDatasetItem item;
    item.setItemId("item-a");
    item.setSourceId("source-a");
    item.setArtistId("artist-a");
    item.setSessionId("session-a");
    item.split = more_phi::NeuralMasteringDatasetSplit::Train;
    item.licenseStatus = more_phi::NeuralMasteringLicenseStatus::Approved;
    item.referenceQuality = more_phi::NeuralMasteringReferenceQuality::Reviewed;
    item.provenanceComplete = true;
    item.hasReferenceMaster = true;
    item.sourceFingerprint = fingerprint;
    item.featureFrame = extracted.frame;
    item.targetVector.eq[0] = 0.1f;
    return item;
}

more_phi::NeuralMasteringModelMetadata validModelMetadata()
{
    more_phi::NeuralMasteringModelMetadata metadata;
    metadata.setModelId("disabled-poc-neural-mastering");
    metadata.setChecksum("metadata-only");
    metadata.addSupportedSampleRate(44100.0);
    metadata.addSupportedSampleRate(48000.0);
    metadata.addSupportedLayout(more_phi::NeuralMasteringLayout::Mono);
    metadata.addSupportedLayout(more_phi::NeuralMasteringLayout::Stereo);
    return metadata;
}

} // namespace

TEST_CASE("NeuralMasteringDatasetSchema validates provenance license split and reference quality", "[NeuralMasteringDataset][US2]")
{
    SECTION("complete approved dataset item is valid")
    {
        const auto result = more_phi::validateNeuralMasteringDatasetItem(validDatasetItem());

        CHECK(result.valid);
        CHECK(result.issueCount == 0);
    }

    SECTION("missing provenance is rejected")
    {
        auto item = validDatasetItem();
        item.provenanceComplete = false;

        const auto result = more_phi::validateNeuralMasteringDatasetItem(item);

        CHECK_FALSE(result.valid);
        CHECK(result.hasIssue(more_phi::NeuralMasteringDatasetValidationIssue::MissingProvenance));
    }

    SECTION("unresolved license is rejected")
    {
        auto item = validDatasetItem();
        item.licenseStatus = more_phi::NeuralMasteringLicenseStatus::Unresolved;

        const auto result = more_phi::validateNeuralMasteringDatasetItem(item);

        CHECK_FALSE(result.valid);
        CHECK(result.hasIssue(more_phi::NeuralMasteringDatasetValidationIssue::LicenseNotApproved));
    }

    SECTION("unreviewed reference quality is rejected")
    {
        auto item = validDatasetItem();
        item.referenceQuality = more_phi::NeuralMasteringReferenceQuality::NeedsReview;

        const auto result = more_phi::validateNeuralMasteringDatasetItem(item);

        CHECK_FALSE(result.valid);
        CHECK(result.hasIssue(more_phi::NeuralMasteringDatasetValidationIssue::ReferenceQualityNotReviewed));
    }

    SECTION("unsupported material is rejected")
    {
        auto item = validDatasetItem();
        item.unsupportedMaterial = true;

        const auto result = more_phi::validateNeuralMasteringDatasetItem(item);

        CHECK_FALSE(result.valid);
        CHECK(result.hasIssue(more_phi::NeuralMasteringDatasetValidationIssue::UnsupportedMaterial));
    }
}

TEST_CASE("NeuralMasteringDatasetSchema enforces split isolation", "[NeuralMasteringDataset][US2]")
{
    auto train = validDatasetItem(3003);
    auto validation = validDatasetItem(3003);
    validation.split = more_phi::NeuralMasteringDatasetSplit::Validation;

    const more_phi::NeuralMasteringDatasetItem items[] = { train, validation };
    const auto result = more_phi::validateNeuralMasteringDatasetSplitIsolation(items, 2);

    CHECK_FALSE(result.valid);
    CHECK(result.hasIssue(more_phi::NeuralMasteringDatasetValidationIssue::SplitIsolationViolation));
}

TEST_CASE("NeuralMasteringDatasetSchema validates disabled model-card metadata fixtures", "[NeuralMasteringDataset][US2]")
{
    SECTION("disabled metadata-only PoC can be structurally valid without enabling runtime inference")
    {
        const auto metadata = validModelMetadata();
        const auto result = more_phi::validateNeuralMasteringModelMetadata(metadata);

        CHECK(result.valid);
        CHECK_FALSE(metadata.enabled);
        CHECK_FALSE(metadata.audioCallbackInference);
        CHECK(metadata.supportsLayout(more_phi::NeuralMasteringLayout::Stereo));
        CHECK(metadata.supportsSampleRate(48000.0));
    }

    SECTION("audio callback inference is rejected in metadata")
    {
        auto metadata = validModelMetadata();
        metadata.audioCallbackInference = true;

        const auto result = more_phi::validateNeuralMasteringModelMetadata(metadata);

        CHECK_FALSE(result.valid);
        CHECK(result.hasIssue(more_phi::NeuralMasteringModelMetadataIssue::AudioCallbackInferenceEnabled));
    }

    SECTION("missing supported layout is rejected")
    {
        auto metadata = validModelMetadata();
        metadata.supportedLayoutCount = 0;

        const auto result = more_phi::validateNeuralMasteringModelMetadata(metadata);

        CHECK_FALSE(result.valid);
        CHECK(result.hasIssue(more_phi::NeuralMasteringModelMetadataIssue::MissingLayoutSupport));
    }
}
