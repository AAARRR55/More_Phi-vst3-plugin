// tests/Unit/TestGenreClassifierRemap.cpp
//
// Phase B: the genre-remap helper. A model's per-class softmax output gets
// remapped onto the plugin's 12 genre slots via GenreClassifier::remapGenreProbs.
// This pins the three contract cases: identity mapping, unmapped argmax (-1 →
// fallback signal), and probability distribution across the 12 slots.
//
// No ONNX / no model file — the helper is pure.
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AI/GenreClassifier.h"

#include <array>
#include <vector>

TEST_CASE("remapGenreProbs: identity mapping routes argmax to same-numbered slot",
          "[GenreClassifier][Remap]")
{
    // Model has 8 classes, identity-mapped (class i → slot i). Model is most
    // confident on class 3 → plugin slot 3 should get the confidence.
    std::vector<float> modelProbs = {0.05f, 0.05f, 0.05f, 0.70f, 0.05f, 0.05f, 0.03f, 0.02f};
    std::vector<int> remap = {0, 1, 2, 3, 4, 5, 6, 7};   // identity for 8 classes
    std::array<float, more_phi::GenreClassifier::kNumGenres> out {};

    const int slot = more_phi::GenreClassifier::remapGenreProbs(
        modelProbs.data(), static_cast<int>(modelProbs.size()),
        remap.data(), static_cast<int>(remap.size()), out.data());

    REQUIRE(slot == 3);
    CHECK(out[3] == Catch::Approx(0.70f).margin(1e-5f));
    // Remaining probability distributed across the other 11 slots.
    const float expectedRest = (1.0f - 0.70f) / 11.0f;
    for (int i = 0; i < 12; ++i)
        if (i != 3) CHECK(out[i] == Catch::Approx(expectedRest).margin(1e-5f));
}

TEST_CASE("remapGenreProbs: unmapped argmax returns -1 (fallback signal)",
          "[GenreClassifier][Remap]")
{
    // Model's top class is index 5, but the remap marks it -1 (the model's class
    // 5 has no plugin-slot equivalent). The helper must signal fallback (-1) so
    // the classifier drops to the heuristic rather than asserting a wrong genre.
    std::vector<float> modelProbs = {0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.45f, 0.05f};
    std::vector<int> remap = {0, 1, 2, 3, 4, -1, 5};   // class 5 unmapped
    std::array<float, more_phi::GenreClassifier::kNumGenres> out {};

    const int slot = more_phi::GenreClassifier::remapGenreProbs(
        modelProbs.data(), static_cast<int>(modelProbs.size()),
        remap.data(), static_cast<int>(remap.size()), out.data());

    CHECK(slot == -1);
}

TEST_CASE("remapGenreProbs: non-identity remap shuffles slots correctly",
          "[GenreClassifier][Remap]")
{
    // Model class 0 → plugin slot 7, class 1 → slot 2, others unmapped.
    // Model is most confident on class 1 → expect plugin slot 2.
    std::vector<float> modelProbs = {0.30f, 0.60f, 0.10f};
    std::vector<int> remap = {7, 2, -1};
    std::array<float, more_phi::GenreClassifier::kNumGenres> out {};

    const int slot = more_phi::GenreClassifier::remapGenreProbs(
        modelProbs.data(), static_cast<int>(modelProbs.size()),
        remap.data(), static_cast<int>(remap.size()), out.data());

    REQUIRE(slot == 2);
    CHECK(out[2] == Catch::Approx(0.60f).margin(1e-5f));
    // The remaining probability (1-0.6)/11 is distributed across all other 11
    // slots, including slot 7 (which class 0 maps to). Slot 7 is NOT zero — it
    // receives its share of the residual. (Class 0's own 0.30 probability does
    // not route to slot 7; only the argmax slot gets the confidence.)
    CHECK(out[7] == Catch::Approx((1.0f - 0.60f) / 11.0f).margin(1e-5f));
}

TEST_CASE("remapGenreProbs: confidence clamps to [0,1]",
          "[GenreClassifier][Remap]")
{
    // A malformed model output above 1.0 must clamp, not propagate.
    std::vector<float> modelProbs = {1.5f, 0.0f};
    std::vector<int> remap = {3, 4};
    std::array<float, more_phi::GenreClassifier::kNumGenres> out {};

    const int slot = more_phi::GenreClassifier::remapGenreProbs(
        modelProbs.data(), 2, remap.data(), 2, out.data());

    REQUIRE(slot == 3);
    CHECK(out[3] == Catch::Approx(1.0f).margin(1e-5f));   // clamped
}
