/*
 * AUDIT (F3.1, 2026-06-27): the live_measurements JSON block must never emit
 * non-finite floats. LUFSMeter / TruePeakEstimator initialse their atomics to
 * -infinity and only become finite once real signal crosses the gates. Before
 * this fix, sonicmasterMeasurementsJson() emitted the raw -inf values, which
 * nlohmann::json serialises as `null` — so the assistant received
 *   {"valid": true, "lufs_integrated": null, "true_peak_dbtp": null, ...}
 * and could not distinguish "engine attached, no signal yet" from "engine
 * failed." Every OTHER metrics path in MCPToolHandler.cpp uses finiteOr(); this
 * is the one block that was missed.
 *
 * This test drives the serializer directly with a snapshot whose lufs/true-peak
 * fields are -infinity (the exact state after prepare() with no audio) and
 * asserts: (a) the emitted numbers are finite, (b) a measured_finite flag
 * reports false so the assistant can disambiguate, (c) a fully-finite snapshot
 * reports measured_finite == true with the real values preserved.
 */
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>

#include "AI/MCPToolHandler.h"
#include "AI/SonicMasterAnalysisEngine.h"

namespace
{
more_phi::SonicMasterMeasurementSnapshot silentAttachedSnapshot()
{
    // Mirror the exact post-prepare() state: engine wired but no signal has
    // crossed the LUFS / true-peak gates, so the meters still hold their
    // -infinity sentinels. valid==true because the engine is attached.
    more_phi::SonicMasterMeasurementSnapshot s;
    const float negInf = -std::numeric_limits<float>::infinity();
    s.lufsIntegrated = negInf;
    s.lufsShortTerm  = negInf;
    s.lufsMomentary  = negInf;
    s.lra            = negInf;
    s.truePeakDbtp   = negInf;
    s.spectralCentroidHz = 0.0f;
    s.spectralTilt   = 0.0f;
    s.stereoWidth    = 0.0f;
    s.correlationMid = 0.0f;
    s.thdPercent     = 0.0f;
    s.crestFactorProgram = 0.0f;
    s.valid          = true;
    return s;
}

more_phi::SonicMasterMeasurementSnapshot liveReadingSnapshot()
{
    more_phi::SonicMasterMeasurementSnapshot s;
    s.lufsIntegrated = -14.0f;
    s.lufsShortTerm  = -13.5f;
    s.lufsMomentary  = -12.0f;
    s.lra            = 5.0f;
    s.truePeakDbtp   = -1.2f;
    s.spectralCentroidHz = 2200.0f;
    s.spectralTilt   = -0.1f;
    s.stereoWidth    = 0.8f;
    s.correlationMid = 0.92f;
    s.thdPercent     = 0.4f;
    s.crestFactorProgram = 11.0f;
    s.valid          = true;
    return s;
}

bool jsonFieldIsFiniteNumber(const nlohmann::json& j, const char* key)
{
    if (!j.contains(key) || j[key].is_null()) return false;
    if (!j[key].is_number()) return false;
    return std::isfinite(j[key].get<double>());
}
} // namespace

TEST_CASE("sonicmaster live_measurements never emits non-finite floats (F3.1)",
          "[mcp][sonicmaster][audit-f3-1]")
{
    SECTION("silent-attached snapshot serialises finite numbers, measured_finite=false")
    {
        const auto snapshot = silentAttachedSnapshot();
        const auto meas = more_phi::MCPToolHandler::serializeMeasurementsForTests(snapshot);

        REQUIRE(meas["valid"].get<bool>() == true);
        // measured_finite lets the assistant disambiguate "engine attached,
        // no signal yet" from a real reading.
        REQUIRE(meas.contains("measured_finite"));
        REQUIRE(meas["measured_finite"].get<bool>() == false);

        // Every numeric field must be a finite number, never null/-inf.
        REQUIRE(jsonFieldIsFiniteNumber(meas, "lufs_integrated"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "lufs_short_term"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "lufs_momentary"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "lra"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "true_peak_dbtp"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "spectral_centroid_hz"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "spectral_tilt"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "stereo_width"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "correlation_mid"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "thd_percent"));
        REQUIRE(jsonFieldIsFiniteNumber(meas, "crest_factor_program"));

        // The finite fallback for an unread LUFS is the same -70 floor the
        // heuristic-input path already uses (MCPToolHandler.cpp:199).
	CHECK_THAT(meas["lufs_integrated"].get<double>(), Catch::Matchers::WithinAbs(-70.0, 1e-6));
	// The finite fallback for an unread true-peak is the -6 dBTP floor
	// matching the heuristic-input convention.
	CHECK_THAT(meas["true_peak_dbtp"].get<double>(), Catch::Matchers::WithinAbs(-6.0, 1e-6));
    }

    SECTION("live reading snapshot preserves real values, measured_finite=true")
    {
        const auto snapshot = liveReadingSnapshot();
        const auto meas = more_phi::MCPToolHandler::serializeMeasurementsForTests(snapshot);

        REQUIRE(meas["valid"].get<bool>() == true);
        REQUIRE(meas["measured_finite"].get<bool>() == true);
	CHECK_THAT(meas["lufs_integrated"].get<double>(), Catch::Matchers::WithinAbs(-14.0, 1e-6));
	CHECK_THAT(meas["true_peak_dbtp"].get<double>(),  Catch::Matchers::WithinAbs(-1.2, 1e-6));
	CHECK_THAT(meas["spectral_centroid_hz"].get<double>(), Catch::Matchers::WithinAbs(2200.0, 1e-3));
	CHECK_THAT(meas["correlation_mid"].get<double>(), Catch::Matchers::WithinAbs(0.92, 1e-6));
        CHECK(meas["measurement_semantics"].get<std::string>()
              == "genuine_input_measurement_NOT_model_estimate");
    }

    SECTION("invalid (engine not attached) snapshot emits valid=false, no numeric block")
    {
        more_phi::SonicMasterMeasurementSnapshot s; // valid==false by default
        const auto meas = more_phi::MCPToolHandler::serializeMeasurementsForTests(s);

        REQUIRE(meas["valid"].get<bool>() == false);
        REQUIRE(meas["measured_finite"].get<bool>() == false);
        // The numeric block is gated behind valid; with valid=false nothing
        // numeric is emitted, so the assistant does not see stale zeros.
        REQUIRE_FALSE(meas.contains("lufs_integrated"));
        REQUIRE_FALSE(meas.contains("true_peak_dbtp"));
    }
}
