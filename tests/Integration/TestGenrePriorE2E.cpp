// tests/Integration/TestGenrePriorE2E.cpp
//
// Phase 4 E2E: verifies the genre-prior wiring is structurally sound inside the
// processor and the documented no-model caveat holds. The full prior mechanics
// (genre → decode → applied plan) are pinned in the unit tests
// (TestGenreLoudnessPrior, TestSonicMasterDecisionDecoder[Stage2]); this test
// pins the WIRING SURFACE: the processor owns both engines, the genre classifier
// defaults to Streaming with no model, and the SonicMaster engine exposes the
// three prior setters/getters the timer pushes through.
//
// (We do NOT invoke the processor's real juce::Timer callback — headless JUCE
// timers need a running message loop, and re-inlining the timer body would just
// test a copy of the logic. The unit tests already cover the decode-side
// behavior; this test confirms the integration points exist and compile.)
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "Plugin/PluginProcessor.h"
#include "AI/GenreMasteringProfile.h"
#include "AI/SonicMasterDecisionDecoder.h"

using Catch::Approx;

TEST_CASE("Genre prior wiring: engines coexist and classifier defaults to Streaming",
          "[integration][GenrePrior]")
{
    more_phi::MorePhiProcessor proc;
    proc.prepareToPlay(48000.0, 512);

    // The processor owns both engines and exposes them — the timer's
    // rendezvous point (autoMasteringEngine_.getGenreClassifier() →
    // sonicMasterEngine_.set*()) compiles and links.
    auto& sm = proc.getSonicMasterEngine();
    auto& classifier = proc.getAutoMasteringEngine().getGenreClassifier();

    // No-model caveat: with no genre ONNX loaded, the classifier returns the
    // Streaming (general) slot at confidence 1.0. The wiring therefore collapses
    // to the Streaming profile until a model is wired — safe, non-destructive.
    REQUIRE(classifier.getTopGenre() == 10);            // Streaming
    REQUIRE(classifier.getTopConfidence() >= 0.5f);     // confident by default

    const auto streaming = more_phi::kGenreMasteringProfiles[10];
    REQUIRE(streaming.targetLufs == Approx(-14.0f).margin(1e-3f));

    // The SonicMaster engine exposes the three prior surfaces the timer pushes.
    // Verify they round-trip (set → get), proving the wiring target is live.
    sm.setGenreTargetLufs(streaming.targetLufs);
    sm.setGenreCurveIndex(5);   // streaming slot in kMasteringTargetCurves
    sm.setResidualBlend(0.5f);
    CHECK(sm.getGenreTargetLufs() == Approx(-14.0f).margin(1e-3f));
    CHECK(sm.getGenreCurveIndex() == 5);
    CHECK(sm.getResidualBlend() == Approx(0.5f).margin(1e-3f));

    // Low-confidence branch clears all three (the timer's else-path).
    sm.setGenreTargetLufs(more_phi::kUseModelTargetLufs);
    sm.setGenreCurveIndex(-1);
    sm.setResidualBlend(0.0f);
    CHECK(sm.getGenreTargetLufs() == more_phi::kUseModelTargetLufs);
    CHECK(sm.getGenreCurveIndex() == -1);
    CHECK(sm.getResidualBlend() == 0.0f);

    proc.releaseResources();
}
