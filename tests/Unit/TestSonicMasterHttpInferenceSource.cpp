// tests/Unit/TestSonicMasterHttpInferenceSource.cpp
//
// Exercises the pure serialization helpers (no network). The live HTTP path is
// verified by an end-to-end probe against a running inference server.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "AI/SonicMasterHttpInferenceSource.h"

#include <string>

TEST_CASE("buildInferRequestBody interleaves L/R into [L0,R0,L1,R1,...]",
          "[SonicMaster][Http]")
{
    constexpr std::size_t n = 4;
    const float l[n] = { 0.10f, 0.20f, 0.30f, 0.40f };
    const float r[n] = { 0.15f, 0.25f, 0.35f, 0.45f };
    float out[2 * n] = {};

    more_phi::buildInferRequestBody(l, r, n, out);

    CHECK_THAT(out[0], Catch::Matchers::WithinAbs(0.10f, 1e-5f));
    CHECK_THAT(out[1], Catch::Matchers::WithinAbs(0.15f, 1e-5f));
    CHECK_THAT(out[2], Catch::Matchers::WithinAbs(0.20f, 1e-5f));
    CHECK_THAT(out[3], Catch::Matchers::WithinAbs(0.25f, 1e-5f));
    CHECK_THAT(out[6], Catch::Matchers::WithinAbs(0.40f, 1e-5f));
    CHECK_THAT(out[7], Catch::Matchers::WithinAbs(0.45f, 1e-5f));
}

TEST_CASE("parseInferResponse extracts the 44-float decision array",
          "[SonicMaster][Http]")
{
    // A representative /infer body with a 44-element decision + extra fields.
    std::string body = R"({"decision":[)";
    for (int i = 0; i < 44; ++i)
        body += std::to_string(static_cast<float>(i) * 0.5f) + ",";
    body.pop_back();  // trailing comma
    body += "],\"inference_ms\":312.5}";

    float dec[more_phi::kSonicMasterDecisionWidth] = { -1.0f };
    REQUIRE(more_phi::parseInferResponse(body, dec, more_phi::kSonicMasterDecisionWidth));

    for (std::size_t i = 0; i < more_phi::kSonicMasterDecisionWidth; ++i)
        CHECK_THAT(dec[i], Catch::Matchers::WithinAbs(static_cast<float>(i) * 0.5f, 1e-3f));
}

TEST_CASE("parseInferResponse rejects malformed payloads",
          "[SonicMaster][Http]")
{
    float dec[more_phi::kSonicMasterDecisionWidth] = {};
    CHECK_FALSE(more_phi::parseInferResponse("", dec, more_phi::kSonicMasterDecisionWidth));
    CHECK_FALSE(more_phi::parseInferResponse(R"({"other":1})", dec, more_phi::kSonicMasterDecisionWidth));
    CHECK_FALSE(more_phi::parseInferResponse(R"({"decision":"notarray"})", dec, more_phi::kSonicMasterDecisionWidth));
    // Too few elements.
    CHECK_FALSE(more_phi::parseInferResponse(R"({"decision":[1,2,3]})", dec, more_phi::kSonicMasterDecisionWidth));
}

TEST_CASE("parseInferResponse handles negative and scientific-notation values",
          "[SonicMaster][Http]")
{
    std::string body = R"({"decision":[)";
    for (std::size_t i = 0; i < more_phi::kSonicMasterDecisionWidth; ++i)
        body += "-1.5e0,";  // all -1.5
    body.pop_back();
    body += "]}";

    float dec[more_phi::kSonicMasterDecisionWidth] = {};
    REQUIRE(more_phi::parseInferResponse(body, dec, more_phi::kSonicMasterDecisionWidth));
    CHECK_THAT(dec[0], Catch::Matchers::WithinAbs(-1.5f, 1e-4f));
    CHECK_THAT(dec[43], Catch::Matchers::WithinAbs(-1.5f, 1e-4f));
}
