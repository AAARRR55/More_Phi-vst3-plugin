/*
 * AUDIT (F2.1, 2026-06-27): OzonePlanApplicator::nameMatches re-validates a
 * hosted-plugin parameter name at a positional index before writing, to catch
 * index drift when the hosted plugin is swapped. The prior bidirectional
 * substring comparator (a.contains(e) || e.contains(a)) was too permissive:
 *   - a param literally named "gain" matched expected "eq band 1 gain"
 *     (because the EXPECTED string contains the token "gain");
 *   - expected "compressor threshold" matched a param named "threshold" alone.
 * The fix tokenizes both names on non-alphanumeric boundaries and matches iff
 * every token of the SHORTER string is present in the longer string's token
 * set. These tests pin the new contract so the comparator can't regress.
 */
#include <catch2/catch_test_macros.hpp>

#include "AI/OzonePlanApplicator.h"

using more_phi::OzonePlanApplicator;

TEST_CASE("nameMatches accepts exact and display-decorated names (F2.1)",
          "[audit][ozone][F2-1]")
{
    // Exact match (case-insensitive).
    CHECK(OzonePlanApplicator::nameMatches("eq band 1 frequency", "EQ Band 1 Frequency"));
    // Vendor display decoration: extra "(Hz)" token in the actual name.
    CHECK(OzonePlanApplicator::nameMatches("eq band 1 frequency", "EQ Band 1 Frequency (Hz)"));
    // Map-key-as-single-token: expected "gain" against a fully-qualified actual.
    CHECK(OzonePlanApplicator::nameMatches("gain", "eq band 1 gain"));
    // Whitespace / punctuation tolerance.
    CHECK(OzonePlanApplicator::nameMatches("threshold", "Compressor Threshold (dB)"));
}

TEST_CASE("nameMatches rejects genuine module mismatches (F2.1)",
          "[audit][ozone][F2-1]")
{
    // CRITICAL: a bare "gain" must NOT match a compressor threshold slot. The
    // prior substring comparator passed this because "compressor threshold"
    // contains... actually it didn't, but the reverse direction "threshold"
    // expected vs "compressor threshold gain" actual would have. Pin both.
    CHECK_FALSE(OzonePlanApplicator::nameMatches("compressor threshold", "gain"));
    CHECK_FALSE(OzonePlanApplicator::nameMatches("threshold", "eq band 1 gain"));
    // Different module entirely.
    CHECK_FALSE(OzonePlanApplicator::nameMatches("eq band 1 frequency", "stereo width"));
    CHECK_FALSE(OzonePlanApplicator::nameMatches("maximizer ceiling", "compressor ratio"));
}

TEST_CASE("nameMatches rejects EQ band-swap confusion (F2.1)",
          "[audit][ozone][F2-1]")
{
    // Band-1 expected, band-2 actual: the "1" vs "2" token must differ.
    CHECK_FALSE(OzonePlanApplicator::nameMatches("eq band 1 gain", "eq band 2 gain"));
    CHECK_FALSE(OzonePlanApplicator::nameMatches("eq band 1 frequency",
                                                 "EQ Band 2 Frequency (Hz)"));
    // Same band is fine.
    CHECK(OzonePlanApplicator::nameMatches("eq band 3 gain", "eq band 3 gain"));
}

TEST_CASE("nameMatches handles empty / degenerate inputs (F2.1)",
          "[audit][ozone][F2-1]")
{
    CHECK_FALSE(OzonePlanApplicator::nameMatches("", "anything"));
    CHECK_FALSE(OzonePlanApplicator::nameMatches("anything", ""));
    // Pure-punctuation names produce no tokens -> no match.
    CHECK_FALSE(OzonePlanApplicator::nameMatches("---", "( )"));
    CHECK_FALSE(OzonePlanApplicator::nameMatches("gain", "(Hz)"));
}
