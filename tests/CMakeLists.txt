# MorePhi — Test Suite (more_phi:: API, Catch2 v3)
# Only compiles real, existing test sources.

cmake_minimum_required(VERSION 3.24)
enable_testing()

# ── Catch2 v3 ──────────────────────────────────────────────────────────────────
if(NOT TARGET Catch2::Catch2WithMain)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.4.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# ── Unit test executable ────────────────────────────────────────────────────────
# Legacy Morphy-era tests under tests/src are intentionally excluded.

add_executable(MorePhiTests
    Unit/test_more_phi_unit.cpp
    Unit/TestPhysicsAndGenetic.cpp
    Unit/TestSidechainTrigger.cpp
    Unit/TestAudioEngine.cpp
    Unit/TestRecallRamp.cpp
    Unit/TestBypassCrossfade.cpp
    Unit/TestDSPQuality.cpp
    # Output-protection limiter on the main wet path (morph-clip fix).
    Unit/TestMorphOutputProtection.cpp
    Unit/TestAIRegressions.cpp
    Unit/TestLLMSettings.cpp
    Unit/TestLLMSettingsStore.cpp
    Unit/TestLLMConnectionValidator.cpp
    Unit/TestLLMChatClient.cpp
    Unit/TestLLMSettingsDialog.cpp
    Unit/TestOfflineBatchRenderer.cpp
    Unit/AudioBufferPoolTests.cpp
    Unit/SIMDAudioTests.cpp
    Unit/PerformanceProfilerTests.cpp
    Integration/TestPluginLifecycle.cpp
    Integration/TestGenrePriorE2E.cpp
    Integration/TestMCPIntegration.cpp
    Integration/TestDatasetIntegration.cpp
    Integration/TestUserManualFeatureSurface.cpp
    Integration/TestVST3ParameterAutomation.cpp
    Integration/TestVST3MidiAndSidechain.cpp
    Integration/TestVST3AudioSignalAccuracy.cpp
    # Phase 1: end-to-end activation (skips without secrets; never fails without them)
    Integration/TestActivationE2E.cpp
    # Integration/TestVST3ComprehensiveE2E.cpp  # Out of sync with v3.3 core/API; needs rewrite
    # V2 subsystem tests
    Unit/TestModulationEngine.cpp
    Unit/TestSpectralEngine.cpp
    Unit/TestGranularEngine.cpp
    Unit/TestPresetLibrary.cpp
    Unit/TestV2Integration.cpp
    # Unit/TestDatasetModules.cpp  # Existing suite is out of sync with current dataset APIs

    # ── Phase 1/2: Gravity Wells & Voronoi Morph Engine ──────────────────────────
    Unit/TestGravityWells.cpp
    Unit/TestVoronoiMorph.cpp

    # ── Phase 7: Smart Preset Caching ───────────────────────────────────────────
    Unit/TestSmartCaching.cpp

    # ── Pre-Deployment Audit Protocol (2026-04-14) ──────────────────────────────
    # Module 2: Host Integration (P0)
    Unit/TestHostIntegration.cpp
    Unit/TestDAWTransportForwarder.cpp
    # Module 3: MIDI Routing (P1)
    Unit/TestMIDIRouting.cpp
    # Module 4: Concurrency & Real-Time Safety (P0)
    Unit/TestConcurrency.cpp
    # Module 5: State Persistence (P0)
    Unit/TestStatePersistence.cpp
    # Module 10: Stress & Edge Cases (P1)
    Unit/TestStressEdgeCases.cpp
    # Module 11: End-to-End Signal Path (P0)
    Unit/TestE2ESignalPath.cpp
    # Module 7: AI / MCP Server (P2)
    Unit/TestMCPServerUnit.cpp
    Unit/TestAutomationControlPlane.cpp
    Unit/TestVST3IPCBridge.cpp
    Unit/TestAsyncToolExecutor.cpp
    # Ozone 11 mastering integration
    Unit/TestMorePhiIpcIntegration.cpp
    # Neural mastering safety projection
    Unit/TestNeuralMasteringSafetyPolicy.cpp
    Unit/TestNeuralMasteringController.cpp
    Unit/TestRuleBasedMasteringResolver.cpp
    Unit/TestNeuralMasteringFeatureExtractor.cpp
    Unit/TestOnnxNeuralMasteringRunner.cpp
    Unit/TestOnnxRunnerInference.cpp
    # SonicMaster realtime neural mastering (preview)
    Unit/TestSonicMasterDecisionDecoder.cpp
    # Contract parsing regression guard (C1: shipped contract schema mismatch).
    # ONNX-independent — runs in every CI config.
    Unit/TestSonicMasterContractParsing.cpp
    # Ozone parameter-map discovery regression guard (W1: silent no-op when the
    # hosted plugin's names don't match the discovery heuristic).
    Unit/TestOzoneParameterMapDiscovery.cpp
    # Per-parameter write-source stamp + write-precedence conflict counter (E2).
    Unit/TestParameterBridgeWriteSource.cpp
    # Hosted-parameter transition guard (C7): notifyHostedParameterChanged
    # discards a straddling capture window + flushes the ring.
    Unit/TestSonicMasterTransitionGuard.cpp
    # C1 empirical proof: exercises the PRODUCTION loadModel(model, contract)
    # path against the real staged ONNX model (the no-contract live test skips
    # the parser, which is exactly how C1 hid). Opt-in: ONNX + staged model.
    Unit/TestSonicMasterRunnerLiveContract.cpp
    Unit/TestAuditFixes.cpp
    Unit/TestAudioCaptureRing.cpp
    Unit/TestNeuralPlanVerification.cpp
    Unit/TestMasteringMeters.cpp
    Unit/TestSonicMasterAnalysisEngine.cpp
    # AUDIT-FIX (L1-5/L1-6, 2026-06-29): CompNorm constant self-consistency +
    # resampler normalization coverage.
    Unit/TestSonicMasterResampler.cpp
    # AUDIT-FIX (P0, 2026-06-27): full-chain E2E signal integrity test.
    Unit/TestEndToEndSignalIntegrity.cpp
    Unit/TestGenreLoudnessPrior.cpp
    Unit/TestTonalBalanceExtractor.cpp
    Unit/TestTransientShaper.cpp
    Unit/TestMelSpectrogram.cpp
    Unit/TestGenreClassifierFallback.cpp
    Unit/TestGenreClassifierRemap.cpp
    Unit/TestSonicMasterHttpInferenceSource.cpp
    Unit/TestSonicMasterHttpLive.cpp
    # AUDIT-F3.1: live_measurements finite-guard regression (no -inf -> null).
    Unit/TestSonicMasterMeasurementsJson.cpp
    # AUDIT-F2.1: tokenized nameMatches comparator regression.
    Unit/TestOzonePlanApplicatorNameMatch.cpp
    # AUDIT-F2.2: P3.10 plan-boundary drain observability contract.
    Unit/TestPlanBoundaryDrain.cpp
    # Realtime mastering analysis
    Unit/TestMeterWindowAccumulator.cpp
    Unit/TestSpectrumAnalyzer.cpp
    Unit/TestStereoFieldAnalyzer.cpp
    Unit/TestTruePeakEstimator.cpp
    Unit/TestDenormals.cpp
    Unit/TestLUFSMeter.cpp
    Unit/TestAdaptiveEQ.cpp
    Unit/TestModulationAndEnvelope.cpp
    Unit/TestLicensing.cpp
    # Spec optimization extensions (heartbeat, scoped cache invalidation, adaptive rate limit)
    Unit/TestSpecOptimizations.cpp
    # Multi-agent orchestration layer
    Unit/TestAgentRuntimeCore.cpp
    Unit/TestAgentIsolation.cpp
    Unit/TestConductorAgent.cpp
    # Phase 3: real REST LLM hardening (retry/validation/tokens), no network
    Unit/TestRestLlmClientHardening.cpp
    Unit/TestAnalysisAgent.cpp
    Unit/TestOptimizationAgent.cpp
    Unit/TestCreativeAgent.cpp
    Unit/TestRealtimeControlAgent.cpp
    Unit/TestQualitySafetyAgent.cpp
    Unit/TestMemoryAgent.cpp
    Unit/TestStructuredAgentLogger.cpp
    Unit/TestAgentE2E.cpp
    Unit/TestRealtimeReactive.cpp
    Unit/TestAgentAudioThreadIsolation.cpp
    # Orchestrator facades and protocols
    Unit/TestAgentOrchestrator.cpp
    Unit/TestMcpProtocol.cpp
    Unit/TestEcosystemConfig.cpp
    Unit/TestSecurityValidator.cpp
)

# Python stdio MCP server integration test. Only compiled once the C++ IPC
# bridge (VST3IPCBridge) from the cpp-bridge-core scope is present.
if(EXISTS "${CMAKE_SOURCE_DIR}/src/AI/VST3IPCBridge.h")
    target_sources(MorePhiTests PRIVATE Integration/TestPythonMCPIntegration.cpp)
endif()

# Link the compiled plugin objects in (shared) so the test binary has access
# to ParameterState, LockFreeQueue, InterpolationEngine, SnapshotBank
target_link_libraries(MorePhiTests PRIVATE
    Catch2::Catch2WithMain
    MorePhi                 # JUCE shared-code target generated by juce_add_plugin
    juce::juce_core           # propagates JUCE include paths to test translation units
    juce::juce_audio_processors
    juce::juce_cryptography   # LicenseEnvelopeCrypto (BlowFish/SHA256) + license store tests
    juce::juce_dsp            # required by OversamplingWrapper and LatencyManager tests
    nlohmann_json::nlohmann_json
)
if(MORE_PHI_HAS_ED25519)
    # TestLicensing.cpp exercises the real Ed25519 path (ed25519_sign /
    # ed25519_verify) directly, so it needs the vendored lib linked in addition
    # to whatever MorePhi already propagates.
    target_compile_definitions(MorePhiTests PRIVATE MORE_PHI_HAVE_ED25519=1)
    target_link_libraries(MorePhiTests PRIVATE more_phi_ed25519)
endif()

target_include_directories(MorePhiTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}   # allows #include "../Mocks/MockV2Interfaces.h" from Unit/
)

target_compile_definitions(MorePhiTests PRIVATE
    MORE_PHI_TEST_MODE=1
    JUCE_UNIT_TESTS=1
    JUCE_STANDALONE_APPLICATION=0
    JUCE_MODAL_LOOPS_PERMITTED=1
)
# When the plugin is built with ONNX enabled, propagate the definition so test
# TUs that assert on the live-inference path compile against the same code, and
# link the ORT runtime so the test binary can load it.
if(MORE_PHI_HAS_ONNX)
    target_compile_definitions(MorePhiTests PRIVATE MORE_PHI_HAS_ONNX=1)
    target_link_libraries(MorePhiTests PRIVATE more_phi::onnxruntime)

    # PluginProcessor.h includes "SonicMasterModelHash.h" (generated/) and
    # "BinaryData.h" (the embedded-model BinaryData target's JuceLibraryCode)
    # under the MORE_PHI_HAS_ONNX guard. The MorePhi (plugin) target gets these
    # as PRIVATE includes; MorePhiTests links the SharedCode lib directly so it
    # must add them itself or the test TUs fail with C1083.
    # AUDIT (2026-06-26): the model is now embedded in the MorePhiFonts BinaryData
    # (one namespace — separate targets collide on namedResourceList), so the
    # BinaryData.h include dir is the fonts target's JuceLibraryCode.
    target_include_directories(MorePhiTests PRIVATE
        "${CMAKE_BINARY_DIR}/generated"
        "${CMAKE_BINARY_DIR}/juce_binarydata_MorePhiFonts/JuceLibraryCode"
    )

    if(WIN32 AND MORE_PHI_ONNX_RUNTIME_LIB)
        add_custom_command(TARGET MorePhiTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MORE_PHI_ONNX_RUNTIME_LIB}" "$<TARGET_FILE_DIR:MorePhiTests>"
            COMMENT "Copying onnxruntime.dll next to test exe"
            VERBATIM)
    endif()

    # ── SonicMaster realtime neural mastering (preview) ──────────────────────
    # The live smoke test compiles only with ONNX linked. It loads the exported
    # masteringbrain_v2 model + contract if they are staged (produced by
    # tools/export_onnx/masteringbrain_to_onnx.py), and skips cleanly otherwise.
    target_sources(MorePhiTests PRIVATE Unit/TestSonicMasterRunnerLive.cpp)

    set(MOREPHI_SONICMASTER_MODEL    "masteringbrain_v2_decision.onnx")
    set(MOREPHI_SONICMASTER_CONTRACT "masteringbrain_v2_contract.json")
    # AUDIT (C1, 2026-06-26): stage from the canonical models/sonicmaster/ dir
    # (which carries the FIXED contract, masterbrain_v2_decision.contract.json),
    # NOT build/sonicmaster/ (a stale export dir from 2026-06-23 that held the
    # pre-C1 broken 283-byte schema). The build/sonicmaster copy_if_different
    # was silently overwriting the fix on every build, making loadModel fail at
    # the contract-parse step (parsed=0). The source contract filename differs
    # (...decision.contract.json) from the staged name (..._contract.json), so
    # copy under the renamed target.
    set(MOREPHI_SONICMASTER_SRC_DIR        "${CMAKE_SOURCE_DIR}/models/sonicmaster")
    set(MOREPHI_SONICMASTER_SRC_CONTRACT   "${MOREPHI_SONICMASTER_SRC_DIR}/masteringbrain_v2_decision.contract.json")
    set(MOREPHI_SONICMASTER_DST_DIR        "$<TARGET_FILE_DIR:MorePhiTests>")
    if(EXISTS "${MOREPHI_SONICMASTER_SRC_DIR}/${MOREPHI_SONICMASTER_MODEL}")
        add_custom_command(TARGET MorePhiTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MOREPHI_SONICMASTER_SRC_DIR}/${MOREPHI_SONICMASTER_MODEL}"
                    "${MOREPHI_SONICMASTER_DST_DIR}/${MOREPHI_SONICMASTER_MODEL}"
            COMMENT "Staging ${MOREPHI_SONICMASTER_MODEL} next to test exe (from models/)"
            VERBATIM)
    endif()
    if(EXISTS "${MOREPHI_SONICMASTER_SRC_CONTRACT}")
        add_custom_command(TARGET MorePhiTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MOREPHI_SONICMASTER_SRC_CONTRACT}"
                    "${MOREPHI_SONICMASTER_DST_DIR}/${MOREPHI_SONICMASTER_CONTRACT}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MOREPHI_SONICMASTER_SRC_CONTRACT}"
                    "${MOREPHI_SONICMASTER_DST_DIR}/masteringbrain_v2_decision.contract.json"
            COMMENT "Staging fixed ${MOREPHI_SONICMASTER_CONTRACT} next to test exe (from models/)"
            VERBATIM)
    endif()
    # ── V2 feature→delta ONNX model staging (optional) ────────────────────────
    # The 63→72 model is staged alongside the test exe so
    # TestOnnxNeuralMasteringRunner and friends can exercise it. Conditional — if
    # no model file exists, the runner stays !available (graceful degradation).
    set(MOREPHI_V2_MODEL "neural_mastering_v2.onnx")
    set(MOREPHI_V2_MODEL_SRC "${CMAKE_SOURCE_DIR}/models/sonicmaster/${MOREPHI_V2_MODEL}")
    if(EXISTS "${MOREPHI_V2_MODEL_SRC}")
        add_custom_command(TARGET MorePhiTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${MOREPHI_V2_MODEL_SRC}"
                "$<TARGET_FILE_DIR:MorePhiTests>/${MOREPHI_V2_MODEL}"
            COMMENT "Staging ${MOREPHI_V2_MODEL} next to test exe (from models/)"
            VERBATIM)
    endif()
    unset(MOREPHI_V2_MODEL)
    unset(MOREPHI_V2_MODEL_SRC)
endif()
if(COMMAND more_phi_apply_build_tuning)
    more_phi_apply_build_tuning(MorePhiTests)    
endif()

# ── Stack size (MSVC: 4 MB, matches plugin build for V2 engine overhead) ─────
if(MSVC)
    target_link_options(MorePhiTests PRIVATE /STACK:4194304)
endif()

# ── Sanitizers (opt-in via parent MORE_PHI_ENABLE_SANITIZERS) ─────────────────
if(MORE_PHI_ENABLE_SANITIZERS AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU"))
    target_compile_options(MorePhiTests PRIVATE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    )
    target_link_options(MorePhiTests PRIVATE
        -fsanitize=address,undefined
    )
endif()

# ── CTest registration ─────────────────────────────────────────────────────────
include(Catch)
catch_discover_tests(MorePhiTests
    TEST_SPEC "*"
)

# ── Standalone stdio MCP server tests ────────────────────────────────────────
add_executable(MorePhiMcpServerTests
    Unit/TestStandaloneMcpServer.cpp
    Unit/TestMorePhiIPCAssistant.cpp
)

target_link_libraries(MorePhiMcpServerTests PRIVATE
    Catch2::Catch2WithMain
    MorePhiMcpCore
    juce::juce_core
    juce::juce_audio_formats
    juce::juce_audio_processors
    nlohmann_json::nlohmann_json
)

target_include_directories(MorePhiMcpServerTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(MorePhiMcpServerTests PRIVATE
    MORE_PHI_TEST_MODE=1
    JUCE_UNIT_TESTS=1
    JUCE_STANDALONE_APPLICATION=1
)

if(COMMAND more_phi_apply_build_tuning)
    more_phi_apply_build_tuning(MorePhiMcpServerTests)
endif()

catch_discover_tests(MorePhiMcpServerTests
    TEST_SPEC "*"
)

# ── Benchmark executable (opt-in) ──────────────────────────────────────────────
option(MORE_PHI_BUILD_BENCHMARKS "Build performance benchmark suite" OFF)
if(MORE_PHI_BUILD_BENCHMARKS)
    add_executable(MorePhiBenchmarks
        Performance/BenchmarkSuite.cpp
    )

    target_link_libraries(MorePhiBenchmarks PRIVATE
        MorePhi
        juce::juce_core
        juce::juce_audio_processors
    )

    if(WIN32)
        target_link_libraries(MorePhiBenchmarks PRIVATE Psapi)
    endif()

    target_include_directories(MorePhiBenchmarks PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    target_compile_definitions(MorePhiBenchmarks PRIVATE
        MORE_PHI_TEST_MODE=1
        JUCE_STANDALONE_APPLICATION=0
    )
    if(COMMAND more_phi_apply_build_tuning)
        more_phi_apply_build_tuning(MorePhiBenchmarks)
    endif()
endif()

# ── Integrated processor benchmark (audit, opt-in) ───────────────────────────
# AUDIT-2026-06-25: IntegratedProcessorBenchmark.cpp drives the REAL
# MorePhiProcessor.processBlock headlessly through the 8 audit scenarios
# (idle / active / sweep / rapid / neural-isolation / neural-in-context /
# combined / combined+ozone). It has its OWN main() — distinct from
# BenchmarkSuite.cpp's — so it cannot share the MorePhiBenchmarks target.
# Built only when BOTH benchmarks + profiling are requested (profiling is
# required for the per-section breakdown to be meaningful).
if(MORE_PHI_BUILD_BENCHMARKS AND MORE_PHI_ENABLE_PROFILING)
    add_executable(MorePhiAuditBenchmark
        Performance/IntegratedProcessorBenchmark.cpp
    )

    target_link_libraries(MorePhiAuditBenchmark PRIVATE
        MorePhi
        juce::juce_core
        juce::juce_events
        juce::juce_audio_processors
    )

    if(WIN32)
        target_link_libraries(MorePhiAuditBenchmark PRIVATE Psapi)
    endif()

    target_include_directories(MorePhiAuditBenchmark PRIVATE
        ${CMAKE_SOURCE_DIR}/src
    )

    target_compile_definitions(MorePhiAuditBenchmark PRIVATE
        MORE_PHI_TEST_MODE=1
        JUCE_STANDALONE_APPLICATION=0
    )
    if(COMMAND more_phi_apply_build_tuning)
        more_phi_apply_build_tuning(MorePhiAuditBenchmark)
    endif()
endif()

# VST3 validation (optional, needs external validator tooling)
if(TARGET MorePhi_VST3)
    set(MORE_PHI_VST3_VALIDATION_PLUGIN
        "$<TARGET_GENEX_EVAL:MorePhi_VST3,$<TARGET_PROPERTY:MorePhi_VST3,JUCE_PLUGIN_ARTEFACT_FILE>>")

    find_program(VST3_VALIDATOR_EXE vst3_validator)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(VST3_VALIDATOR_EXE AND Python3_Interpreter_FOUND)
        add_test(NAME VST3Validation
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/scripts/run_vst3_validator.py
                --validator ${VST3_VALIDATOR_EXE}
                --plugin ${MORE_PHI_VST3_VALIDATION_PLUGIN}
                --output ${CMAKE_BINARY_DIR}/vst3_validator_results.json
                --junit ${CMAKE_BINARY_DIR}/vst3_validator_junit.xml
                --strict
        )
        set_tests_properties(VST3Validation PROPERTIES TIMEOUT 300 LABELS validation)
    endif()

    find_program(PLUGINVAL_EXE pluginval)
    if(PLUGINVAL_EXE)
        add_test(NAME PluginvalValidation
            COMMAND ${PLUGINVAL_EXE}
                --strictness-level 5
                --validate ${MORE_PHI_VST3_VALIDATION_PLUGIN}
        )
        set_tests_properties(PluginvalValidation PROPERTIES TIMEOUT 300 LABELS validation)
    endif()
endif()

if(APPLE)
    find_program(AUVAL_EXE auval)
    if(AUVAL_EXE)
        add_test(NAME AUValidation
            COMMAND ${AUVAL_EXE} -v aufx Mrph Mrph
        )
        set_tests_properties(AUValidation PROPERTIES TIMEOUT 120 LABELS validation)
    endif()
endif()
