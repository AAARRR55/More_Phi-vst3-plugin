cmake_minimum_required(VERSION 3.24)
project(MorePhi VERSION 3.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(MORE_PHI_COPY_PLUGIN_AFTER_BUILD "Copy plugin bundle to system folder after build" OFF)
option(MORE_PHI_BUILD_TESTS "Configure and build tests in tests/." ON)
option(MORE_PHI_ENABLE_SANITIZERS "Enable ASAN/UBSAN in Debug builds (Clang/GCC only)" OFF)
option(MORE_PHI_SAFE_BUILD_MODE "Enable conservative build settings for local stability" ON)
# Production Ed25519 public key injection (Phase 1 licensing ship-blocker).
# The plugin ships with the DEV keypair's public half in src/Licensing/SigningKeys.cpp
# so local end-to-end activation tests work against a dev backend. A genuine shipping
# build MUST inject the production public key via:
#   cmake -DMORE_PHI_PROD_ED25519_KEY_HEX=8e1aa28c... (64 hex chars, no separators)
# When set, CMake generates a configured header that overrides the dev key, and the
# compile-time static_assert in SigningKeys.cpp passes for Release. When unset, the
# dev key remains and Release builds are correctly blocked (tests/Debug exempt).
# NEVER commit the production key — inject it at build time from CI secrets.
set(MORE_PHI_PROD_ED25519_KEY_HEX "" CACHE STRING "Production Ed25519 public key (64 hex chars) injected at build time; empty = use dev key (Release builds blocked)")
# /MP process count: defaults to the host's logical-core count so MSVC compiles
# that many .cpp files in parallel. Was hard-capped at 2, leaving most cores idle.
# Override per-machine with -DMORE_PHI_MSVC_MP=N (0 disables /MP = serial).
# ponytail: no new option, no default machinery — std::thread::hardware_concurrency
# is the right default and already available in the CMake context.
include(ProcessorCount)
ProcessorCount(N)
if(N LESS 1)
    set(N 4)   # sane fallback if detection fails
endif()
set(MORE_PHI_MSVC_MP "${N}" CACHE STRING "MSVC /MP process count (set 0 to disable /MP).")
option(MORE_PHI_ENABLE_LTO "Enable LTO for Release builds" OFF)
option(MORE_PHI_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
option(MORE_PHI_ENABLE_ONNX "Link ONNX Runtime and enable the OnnxNeuralMasteringRunner inference path (downloads a prebuilt binary at configure time)" OFF)
option(MORE_PHI_ENABLE_PROFILING "Enable PerformanceProfiler instrumentation and the MorePhiDiagnostics 10s message-thread sampler (writes diagnostics-<pid>.log)" OFF)

# AUDIT-2026-06-25: security / commercial-viability opt-ins.
# The embedded MCP server and the standalone Python IPC bridge expose a
# localhost-only JSON-RPC surface. They are enabled by default for Debug and
# test builds (so the existing test suite keeps working), but default OFF for
# Release builds to minimise attack surface. Explicit -D overrides are honored.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(_MORE_PHI_MCP_DEFAULT OFF)
else()
    set(_MORE_PHI_MCP_DEFAULT ON)
endif()
option(MORE_PHI_ENABLE_MCP_SERVER "Start the embedded MCP server and VST3 IPC bridge" ${_MORE_PHI_MCP_DEFAULT})

# The HTTP inference fallback contacts 127.0.0.1:8765 when the bundled ONNX
# model is unavailable. It is opt-in because it silently depends on an external
# server process and leaks traffic even on localhost.
option(MORE_PHI_ENABLE_SONICMASTER_HTTP_FALLBACK "Allow SonicMaster to fall back to the local HTTP inference server" OFF)

set(MORE_PHI_ONNX_VERSION "1.22.1" CACHE STRING "ONNX Runtime prebuilt release version (used when MORE_PHI_ENABLE_ONNX=ON)")
# Pinned at 1.22.1: GetShape() returned-by-value path segfaults against this
# exported graph's ONNX Runtime cxx headers; the runner uses a raw C-API
# invocation as a workaround (see SonicMasterDecisionRunner.cpp:89-99).

if(MORE_PHI_ENABLE_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "clang-tidy")
endif()

# Keep single-config generators deterministic when callers omit build type.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

# Windows: Global preprocessor definitions to prevent macro conflicts
# Must be set BEFORE any Windows headers are included (via JUCE or otherwise)
if(WIN32)
    add_compile_definitions(
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

if(MSVC AND NOT MORE_PHI_MSVC_MP MATCHES "^[0-9]+$")
    message(FATAL_ERROR "MORE_PHI_MSVC_MP must be numeric (for example: 0, 1, 2, 4).")
endif()

# Windows: 4 MB stack for plugin-in-plugin hosting (FL Studio compat)
if(WIN32)
    if(MSVC)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /STACK:4194304")
    elseif(MINGW)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--stack,4194304")
    endif()
endif()

function(more_phi_apply_build_tuning target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "Cannot tune build target '${target_name}' because it does not exist.")
    endif()

    get_target_property(_more_phi_target_type ${target_name} TYPE)
    if(_more_phi_target_type STREQUAL "INTERFACE_LIBRARY" OR _more_phi_target_type STREQUAL "UTILITY")
        return()
    endif()

    if(MSVC)
        # H7 FIX: Undefine Windows macros that collide with JUCE enum names
        # (none, small, large, min, max, normal, high) instead of patching JUCE
        # source files in-place, which breaks hermetic builds.
        target_compile_options(${target_name} PRIVATE /Umin /Umax /Unone /Usmall /Ularge /Unormal /Uhigh)
        # Ensure source files are interpreted as UTF-8 (required for Unicode literals)
        target_compile_options(${target_name} PRIVATE /utf-8)
        # Keep PCH memory and CL internal workers explicit so local machines can tune safely.
        target_compile_options(${target_name} PRIVATE /Zm200)
        if(NOT MORE_PHI_MSVC_MP STREQUAL "0")
            target_compile_options(${target_name} PRIVATE "/MP${MORE_PHI_MSVC_MP}")
        endif()

        if(MORE_PHI_ENABLE_LTO)
            target_compile_options(${target_name} PRIVATE $<$<CONFIG:Release>:/GL>)
        endif()

        if(
            _more_phi_target_type STREQUAL "EXECUTABLE"
            OR _more_phi_target_type STREQUAL "SHARED_LIBRARY"
            OR _more_phi_target_type STREQUAL "MODULE_LIBRARY"
        )
            if(MORE_PHI_SAFE_BUILD_MODE)
                target_link_options(${target_name} PRIVATE /cgthreads:2)
            endif()

            if(MORE_PHI_ENABLE_LTO)
                target_link_options(${target_name} PRIVATE $<$<CONFIG:Release>:/LTCG>)
            endif()
        endif()
    elseif(MORE_PHI_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT more_phi_ipo_supported OUTPUT more_phi_ipo_output)
        if(more_phi_ipo_supported)
            set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        else()
            message(WARNING "LTO requested but IPO is not supported for ${target_name}: ${more_phi_ipo_output}")
        endif()
    endif()
endfunction()

# ── JUCE 8 via FetchContent ───────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(juce)

# ── nlohmann/json (header-only) ──────────────────────────────────────────────
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# ── Ed25519 license-certificate verification ─────────────────────────────────
# The plugin verifies activation-certificate signatures with a self-contained,
# public-domain Ed25519 implementation vendored under third_party/ed25519
# (orlp/ed25519). Verify+sign+keypair paths only. This deliberately avoids a
# libsodium/system-crypto dependency: pulling sodium into a VST3 via
# FetchContent proved unreliable across the available MSVC forks, and platform
# crypto (CNG/Security.framework) would split the verify path across three OSes
# for ~600 lines of portable code. The verifier fails closed if the vendored
# sources are somehow absent (see Ed25519Verifier.cpp).
set(MORE_PHI_ED25519_SOURCES
    third_party/ed25519/src/sha512.c
    third_party/ed25519/src/fe.c
    third_party/ed25519/src/ge.c
    third_party/ed25519/src/sc.c
    third_party/ed25519/src/keypair.c
    third_party/ed25519/src/sign.c
    third_party/ed25519/src/verify.c
)
add_library(more_phi_ed25519 STATIC ${MORE_PHI_ED25519_SOURCES})
target_include_directories(more_phi_ed25519 PUBLIC third_party/ed25519/src)
set_target_properties(more_phi_ed25519 PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(MORE_PHI_HAS_ED25519 1)

# Patch JUCE headers that conflict with Windows macros on MSVC
include(cmake/PatchJuceForMSVC.cmake)

# ── ONNX Runtime (optional, gated) ────────────────────────────────────────────
# When MORE_PHI_ENABLE_ONNX=ON, download the official prebuilt ONNX Runtime
# release for the current platform/arch into a CMake cache directory and expose
# it as an imported target `more_phi::onnxruntime`. The prebuilt-binary route is
# deliberately chosen over building ORT from source: source builds pull in onnx
# + protobuf and add ~30+ minutes to configure, and the prebuilt static/dynamic
# libs are the supported distribution for plugin embedding. Pin to ORT 1.22.1 —
# later versions changed GetShape() semantics (see SonicMasterDecisionRunner.cpp:88-99).
#
# When OFF (default), none of this runs, the build is identical to pre-ONNX, and
# OnnxNeuralMasteringRunner abstains (loadModel returns false).
set(MORE_PHI_HAS_ONNX 0)
if(MORE_PHI_ENABLE_ONNX)
    include(cmake/FetchOnnxRuntime.cmake)
    set(MORE_PHI_HAS_ONNX 1)
endif()

# ── SonicMaster ONNX model (bundled binary data) ──────────────────────────────
# When MORE_PHI_ENABLE_ONNX=ON, embed the ONNX model and contract into the
# plugin binary via juce_add_binary_data so initializeSonicMaster() can extract
# the model at runtime without relying on a sibling file on disk. The expected
# model file is models/sonicmaster/masteringbrain_v2_decision.onnx and its
# sibling contract. Fail-closed at configure time if either file is absent.
if(MORE_PHI_HAS_ONNX)
    set(MOREPHI_SONICMASTER_MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/models/sonicmaster")
    set(MOREPHI_SONICMASTER_MODEL    "${MOREPHI_SONICMASTER_MODEL_DIR}/masteringbrain_v2_decision.onnx")
    set(MOREPHI_SONICMASTER_CONTRACT "${MOREPHI_SONICMASTER_MODEL_DIR}/masteringbrain_v2_decision.contract.json")

    if(NOT EXISTS "${MOREPHI_SONICMASTER_MODEL}")
        message(FATAL_ERROR "SonicMaster ONNX model not found at ${MOREPHI_SONICMASTER_MODEL}. "
            "Place masteringbrain_v2_decision.onnx in models/sonicmaster/ or set MORE_PHI_ENABLE_ONNX=OFF.")
    endif()
    if(NOT EXISTS "${MOREPHI_SONICMASTER_CONTRACT}")
        message(FATAL_ERROR "SonicMaster ONNX contract not found at ${MOREPHI_SONICMASTER_CONTRACT}. "
            "Place masteringbrain_v2_decision.contract.json in models/sonicmaster/ or set MORE_PHI_ENABLE_ONNX=OFF.")
    endif()

    juce_add_binary_data(MorePhiSonicMasterModel
        SOURCES
            "${MOREPHI_SONICMASTER_MODEL}"
            "${MOREPHI_SONICMASTER_CONTRACT}"
    )

    # Generate a hash header at configure time so the runtime can verify
    # the extracted model file matches the embedded source.
    include(cmake/GenerateModelHash.cmake)
    generate_model_hash("${MOREPHI_SONICMASTER_MODEL}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/SonicMasterModelHash.h")
endif()


# Plugin formats: AU is macOS-only, do not emit it on other platforms.
# AAX (Pro Tools) is opt-in: pass -DMORE_PHI_BUILD_AAX=ON with either the AAX
# SDK installed at the JUCE-expected location OR -DAAX_SDK_PATH=/path/to/aax-sdk.
# The SDK is fetched via FetchContent below when the option is on and no path is
# supplied. (Building AAX needs no iLok; only SIGNING/distributing does.)
option(MORE_PHI_BUILD_AAX "Build the AAX (Pro Tools) plugin format" OFF)

if(APPLE)
    set(MORE_PHI_PLUGIN_FORMATS VST3 AU Standalone)
else()
    set(MORE_PHI_PLUGIN_FORMATS VST3 Standalone)
endif()

if(MORE_PHI_BUILD_AAX)
    list(APPEND MORE_PHI_PLUGIN_FORMATS AAX)
    # AAX_SDK_PATH may be supplied by the user, or set by the FetchContent block
    # below. JUCE reads it via JUCE_AAX_SDK_PATH.
    if(DEFINED AAX_SDK_PATH AND NOT DEFINED JUCE_AAX_SDK_PATH)
        set(JUCE_AAX_SDK_PATH "${AAX_SDK_PATH}" CACHE PATH "Path to the AAX SDK" FORCE)
    endif()
endif()

# ── Embedded Fonts (BinaryData) ───────────────────────────────────────────────
# Bundle the landing-page brand typefaces so the GUI renders identically on every
# machine (Syncopate = wordmark/section labels, Outfit = body/value text).
juce_add_binary_data(MorePhiFonts
    SOURCES
        assets/fonts/Syncopate-Bold.ttf
        assets/fonts/Syncopate-Regular.ttf
        assets/fonts/Outfit-Regular.ttf
        assets/fonts/Outfit-SemiBold.ttf
)

# ── Plugin Target ─────────────────────────────────────────────────────────────
# AAX requires the Avid AAX SDK. It is license-gated (free, but requires an Avid
# account + acceptance), so it cannot be auto-fetched like JUCE. Fail closed with
# a clear instruction when AAX is requested but no SDK path resolves.
if(MORE_PHI_BUILD_AAX)
    if(NOT DEFINED JUCE_AAX_SDK_PATH OR JUCE_AAX_SDK_PATH STREQUAL "" OR NOT EXISTS "${JUCE_AAX_SDK_PATH}")
        message(FATAL_ERROR
            "MORE_PHI_BUILD_AAX=ON but the AAX SDK was not found.\n"
            "Get the SDK from Avid (https://developer.avid.com/aax/downloading-the-aax-sdk.htm),\n"
            "then configure with: cmake -B build -S . -DMORE_PHI_BUILD_AAX=ON -DAAX_SDK_PATH=/path/to/aax-sdk")
    endif()
    message(STATUS "AAX format enabled; SDK at ${JUCE_AAX_SDK_PATH}")
endif()

juce_add_plugin(MorePhi
    COMPANY_NAME            "MorePhi"
    PLUGIN_MANUFACTURER_CODE Mrph
    PLUGIN_CODE              Mrps
    FORMATS                  ${MORE_PHI_PLUGIN_FORMATS}
    PRODUCT_NAME             "MorePhi"
    VERSION                  ${PROJECT_VERSION}
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD  ${MORE_PHI_COPY_PLUGIN_AFTER_BUILD}
    VST3_CATEGORIES          "Fx" "Tools"
    # AAX plug-in category (Pro Tools). "Effect" maps to the AAX ePlugInCategory_PhaseVocoder
    # group; the multi-agent/realtime-control surface does not need a separate category.
    AAX_CATEGORIES           "Effect"
)

# ── Source Files ──────────────────────────────────────────────────────────────
# Phase 1: Foundation
set(MORE_PHI_PLUGIN_SOURCES
    src/Plugin/PluginProcessor.h
    src/Plugin/PluginProcessor.cpp
    src/Plugin/PluginEditor.h
    src/Plugin/PluginEditor.cpp
    src/Version.cpp
)

set(MORE_PHI_CORE_SOURCES
    src/Core/ParameterState.h
    src/Core/SnapshotBank.h
    src/Core/SnapshotBank.cpp
    src/Core/InterpolationEngine.h
    src/Core/InterpolationEngine.cpp
    src/Core/PhysicsEngine.h
    src/Core/PhysicsEngine.cpp
    src/Core/GeneticEngine.h
    src/Core/GeneticEngine.cpp
    src/Core/MorphProcessor.h
    src/Core/MorphProcessor.cpp
    src/Core/VoronoiMorphEngine.h
    src/Core/VoronoiMorphEngine.cpp
    src/Core/WaypointEngine.h
    src/Core/LockFreeQueue.h
    src/Core/ThreadPool.h
    src/Core/ThreadPool.cpp
    src/Core/AudioBufferPool.h
    src/Core/AudioBufferPool.cpp
    src/Core/SIMDAudio.h
    src/Core/SIMDAudio.cpp
    src/Core/PerformanceProfiler.h
    src/Core/PerformanceProfiler.cpp
    src/Core/MorePhiDiagnostics.h
    src/Core/MorePhiDiagnostics.cpp
    src/Core/ParameterClassifier.h
    src/Core/ParameterClassifier.cpp
    src/Core/DiscreteParameterHandler.h
    src/Core/DiscreteParameterHandler.cpp
    src/Core/GrainPool.h
    src/Core/GranularMorphEngine.h
    src/Core/GranularMorphEngine.cpp
    src/Core/HybridBlend.h
    src/Core/TransientDetector.h
    src/Core/SpectralTypes.h
    src/Core/SpectralMorphEngine.h
    src/Core/SpectralMorphEngine.cpp
    src/Core/FormantMorphEngine.h
    src/Core/FormantMorphEngine.cpp
    src/Core/ModulationTypes.h
    src/Core/ModulationEngine.h
    src/Core/ModulationEngine.cpp
    src/Core/LFO.h
    src/Core/LFO.cpp
    src/Core/EnvelopeFollower.h
    src/Core/EnvelopeFollower.cpp
    src/Core/StepSequencer.h
    src/Core/StepSequencer.cpp
    src/Core/ModulationMatrix.h
    src/Core/ModulationMatrix.cpp
    src/Core/OversamplingWrapper.h
    src/Core/LatencyManager.h
    src/Core/UndoRedoManager.h
    src/Core/UndoRedoManager.cpp
)

# ── Mastering chain sources (plugin + tests only, not CLI) ────────────────────
set(MORE_PHI_NEURAL_MASTERING_SOURCES
    src/Core/NeuralMasteringTypes.h
    src/Core/NeuralMasteringSafetyPolicy.h
    src/Core/NeuralMasteringSafetyPolicy.cpp
    src/AI/OnnxNeuralMasteringRunner.h
    src/AI/OnnxNeuralMasteringRunner.cpp
    src/AI/SonicMasterDecisionDecoder.h
    src/AI/SonicMasterDecisionDecoder.cpp
    src/AI/SonicMasterDecisionRunner.h
    src/AI/SonicMasterDecisionRunner.cpp
    src/AI/SonicMasterAnalysisEngine.h
    src/AI/SonicMasterAnalysisEngine.cpp
    src/AI/SonicMasterHttpInferenceSource.h
    src/AI/SonicMasterHttpInferenceSource.cpp
)

set(MORE_PHI_MASTERING_SOURCES
    src/Core/MSMatrix.h
    src/Core/MultibandSplitter.h
    src/Core/MultibandSplitter.cpp
    src/Core/MultibandDynamicsProcessor.h
    src/Core/MultibandDynamicsProcessor.cpp
    src/Core/AdaptiveEQ.h
    src/Core/AdaptiveEQ.cpp
    src/Core/StereoImager.h
    src/Core/StereoImager.cpp
    src/Core/HarmonicExciter.h
    src/Core/HarmonicExciter.cpp
    src/Core/TruePeakEstimator.h
    src/Core/TruePeakEstimator.cpp
    src/Core/BrickwallLimiter.h
    src/Core/BrickwallLimiter.cpp
    src/Core/LUFSMeter.h
    src/Core/LUFSMeter.cpp
    src/Core/MeterWindowAccumulator.h
    src/Core/MeterWindowAccumulator.cpp
    src/Core/RealtimeSpectrumAnalyzer.h
    src/Core/RealtimeSpectrumAnalyzer.cpp
    src/Core/StereoFieldAnalyzer.h
    src/Core/StereoFieldAnalyzer.cpp
    src/Core/LoudnessNormalizer.h
    src/Core/LoudnessNormalizer.cpp
    src/Core/MonoCompatibilityChecker.h
    src/Core/AutoMasteringEngine.h
    src/Core/AutoMasteringEngine.cpp
    ${MORE_PHI_NEURAL_MASTERING_SOURCES}
)

set(MORE_PHI_HOST_SOURCES
    src/Host/IPluginHostManager.h
    src/Host/PluginHostManager.h
    src/Host/PluginHostManager.cpp
    src/Host/ParameterBridge.h
    src/Host/ParameterBridge.cpp
)

set(MORE_PHI_LICENSING_SOURCES
    src/Licensing/LicenseTypes.h
    src/Licensing/LicenseTypes.cpp
    src/Licensing/LicenseKey.h
    src/Licensing/LicenseKey.cpp
    src/Licensing/LicenseVerifier.h
    src/Licensing/LicenseVerifier.cpp
    src/Licensing/SecureLicenseStore.h
    src/Licensing/SecureLicenseStore.cpp
    src/Licensing/LicenseEnvelopeCrypto.h
    src/Licensing/LicenseEnvelopeCrypto.cpp
    # Windows DPAPI wrapper (compiled only on Windows; the .cpp self-gates on JUCE_WINDOWS)
    src/Licensing/LicenseDpapiWindows.cpp
    src/Licensing/MachineFingerprint.h
    src/Licensing/MachineFingerprint.cpp
    src/Licensing/ActivationClient.h
    src/Licensing/ActivationClient.cpp
    src/Licensing/LicenseManager.h
    src/Licensing/LicenseManager.cpp
    src/Licensing/SigningKeys.h
    src/Licensing/SigningKeys.cpp
    src/Licensing/Ed25519Verifier.h
    src/Licensing/Ed25519Verifier.cpp
)

set(MORE_PHI_AI_DATASET_STABLE_SOURCES
    src/AI/Dataset/ParameterSampler.h
    src/AI/Dataset/ParameterSampler.cpp
    src/AI/Dataset/AudioContentLibrary.h
    src/AI/Dataset/AudioContentLibrary.cpp
    src/AI/Dataset/PluginChainEngine.h
    src/AI/Dataset/PluginChainEngine.cpp
    src/AI/Dataset/EnhancedRenderPipeline.h
    src/AI/Dataset/EnhancedRenderPipeline.cpp
    src/AI/Dataset/FeatureExtractor.h
    src/AI/Dataset/FeatureExtractor.cpp
    src/AI/Dataset/MetadataWriter.h
    src/AI/Dataset/MetadataWriter.cpp
    src/AI/Dataset/MetadataSchema.cpp
    src/AI/Dataset/ValidationEngine.h
    src/AI/Dataset/ValidationEngine.cpp
    src/AI/Dataset/DatasetOrganizer.h
    src/AI/Dataset/DatasetOrganizer.cpp
    src/AI/Dataset/DatasetGeneratorV2.h
    src/AI/Dataset/DatasetGeneratorV2.cpp
    src/AI/Dataset/OfflineBatchRenderer.h
    src/AI/Dataset/OfflineBatchRenderer.cpp
    src/AI/Dataset/ParameterSafetyConfig.h
    src/AI/Dataset/ParameterSafetyConfig.cpp
    src/AI/Dataset/NeuralMasteringFeatureExtractor.h
    src/AI/Dataset/NeuralMasteringFeatureExtractor.cpp
)

set(MORE_PHI_AI_DATASET_V3_SOURCES
    src/AI/Dataset/DatasetGeneratorV3.h
    src/AI/Dataset/DatasetGeneratorV3.cpp
    src/AI/Dataset/Scheduling/TaskQueue.h
    src/AI/Dataset/Scheduling/WorkerPool.h
    src/AI/Dataset/Monitoring/ResourceMonitor.h
    src/AI/Dataset/Monitoring/ProgressTracker.h
    src/AI/Dataset/Monitoring/GenerationLogger.h
    src/AI/Dataset/Recovery/CheckpointManager.h
    src/AI/Dataset/Recovery/WatchdogTimer.h
)

set(MORE_PHI_AI_SOURCES
    # ── Multi-agent orchestration layer (added per-task; see plan) ────────────
    src/AI/Orchestrator/AgentOrchestrator.h
    src/AI/Orchestrator/AgentOrchestrator.cpp
    src/AI/Orchestrator/McpProtocol.h
    src/AI/Orchestrator/McpProtocol.cpp
    src/AI/Orchestrator/EcosystemConfig.h
    src/AI/Orchestrator/EcosystemConfig.cpp
    src/AI/Orchestrator/SecurityValidator.h
    src/AI/Orchestrator/SecurityValidator.cpp
    src/AI/Agents/Scheduler/PriorityScheduler.h
    src/AI/Agents/Scheduler/PriorityScheduler.cpp
    src/AI/Agents/Blackboard/BlackboardBridge.h
    src/AI/Agents/Blackboard/BlackboardBridge.cpp
    src/AI/Agents/Llm/DeterministicFallbackLlmClient.h
    src/AI/Agents/Llm/DeterministicFallbackLlmClient.cpp
    src/AI/Agents/Llm/RestLlmClient.h
    src/AI/Agents/Llm/RestLlmClient.cpp
    src/AI/Agents/Tooling/DefaultToolInvoker.h
    src/AI/Agents/Tooling/DefaultToolInvoker.cpp
    src/AI/Agents/AgentRegistry.h
    src/AI/Agents/AgentRegistry.cpp
    src/AI/Agents/AgentRuntime.h
    src/AI/Agents/AgentRuntime.cpp
    src/AI/Agents/Conductor/ConductorAgent.h
    src/AI/Agents/Conductor/ConductorAgent.cpp
    src/AI/Agents/Agents/AnalysisAgent.h
    src/AI/Agents/Agents/AnalysisAgent.cpp
    src/AI/Agents/Agents/OptimizationAgent.h
    src/AI/Agents/Agents/OptimizationAgent.cpp
    src/AI/Agents/Agents/CreativeAgent.h
    src/AI/Agents/Agents/CreativeAgent.cpp
    src/AI/Agents/Agents/RealtimeControlAgent.h
    src/AI/Agents/Agents/RealtimeControlAgent.cpp
    src/AI/Agents/Agents/QualitySafetyAgent.h
    src/AI/Agents/Agents/QualitySafetyAgent.cpp
    src/AI/Agents/Agents/MemoryAgent.h
    src/AI/Agents/Agents/MemoryAgent.cpp
    src/AI/Agents/Logging/StructuredAgentLogger.h
    src/AI/Agents/Logging/StructuredAgentLogger.cpp
    src/AI/InstanceIdentity.h
    src/AI/InstanceRegistry.h
    src/AI/InstanceRegistry.cpp
    src/AI/LLMSettings.h
    src/AI/LLMSettings.cpp
    src/AI/LLMSettingsStore.h
    src/AI/LLMSettingsStore.cpp
    src/AI/LLMConnectionValidator.h
    src/AI/LLMConnectionValidator.cpp
    src/AI/LLMChatClient.h
    src/AI/LLMChatClient.cpp
    src/AI/AIAssistant.h
    src/AI/AIAssistant.cpp
    src/AI/EQParameterTranslator.h
    src/AI/EQParameterTranslator.cpp
    src/AI/GenreClassifier.h
    src/AI/GenreClassifier.cpp
    src/AI/ChainPlanExecutor.h
    src/AI/ChainPlanExecutor.cpp
    src/AI/MasteringCandidateScoring.h
    src/AI/NeuralMasteringModelMetadata.h
    src/AI/NeuralMasteringModelRunner.h
    src/AI/NeuralMasteringModelRunner.cpp
    src/AI/NeuralMasteringController.h
    src/AI/NeuralMasteringController.cpp
    src/AI/AutomationControlPlane.h
    src/AI/AutomationControlPlane.cpp
    src/AI/PluginProfileDB.h
    src/AI/PluginProfileDB.cpp
    src/AI/SemanticPluginProfile.h
    src/AI/SemanticPluginProfile.cpp
    src/AI/PluginSemanticMapper.h
    src/AI/PluginSemanticMapper.cpp
    src/AI/TrackAssistantStore.h
    src/AI/TrackAssistantStore.cpp
    src/AI/MCPServer.h
    src/AI/MCPServer.cpp
    src/AI/MCPToolHandler.h
    src/AI/MCPToolHandler.cpp
    src/AI/ToolResultCache.h
    src/AI/ToolResultCache.cpp
    src/AI/AsyncToolExecutor.h
    src/AI/AsyncToolExecutor.cpp
    src/AI/LinkBroadcaster.h
    src/AI/LinkBroadcaster.cpp
    src/AI/TokenOptimizer.h
    src/AI/TokenOptimizer.cpp
    src/AI/MCPToolsExtended.h
    src/AI/MCPToolsExtended.cpp
    src/AI/VST3IPCBridge.h
    src/AI/VST3IPCBridge.cpp
    src/AI/OzoneParameterMap.h
    src/AI/OzoneParameterMap.cpp
    src/AI/OzonePlanApplicator.h
    src/AI/OzonePlanApplicator.cpp
    src/AI/StandaloneMcp/MorePhiPluginBackend.h
    src/AI/StandaloneMcp/MorePhiIPCAssistant.h
    src/AI/StandaloneMcp/MorePhiIPCAssistant.cpp
    src/AI/StandaloneMcp/MorePhiIPCDiscovery.h
    src/AI/StandaloneMcp/MorePhiIPCDiscovery.cpp
    ${MORE_PHI_AI_DATASET_STABLE_SOURCES}
    ${MORE_PHI_AI_DATASET_V3_SOURCES}
)

set(MORE_PHI_MIDI_SOURCES
    src/MIDI/MIDIRouter.h
    src/MIDI/MIDIRouter.cpp
)

set(MORE_PHI_PRESET_SOURCES
    src/Preset/PresetSerializer.h
    src/Preset/PresetSerializer.cpp
    src/Preset/PresetEntry.h
    src/Preset/PresetSerializerV2.h
    src/Preset/PresetSerializerV2.cpp
    src/Preset/PresetLibrary.h
    src/Preset/PresetLibrary.cpp
)

set(MORE_PHI_UI_SOURCES
    src/UI/MorePhiLookAndFeel.h
    src/UI/MorePhiLookAndFeel.cpp
    src/UI/LicenseActivationOverlay.h
    src/UI/LicenseActivationOverlay.cpp
    src/UI/LicenseImportDialog.h
    src/UI/LicenseImportDialog.cpp
    src/UI/ChatDisplay.h
    src/UI/ChatDisplay.cpp
    src/UI/AIChatPanel.h
    src/UI/AIChatPanel.cpp
    src/UI/LLMSettingsDialog.h
    src/UI/LLMSettingsDialog.cpp
    src/UI/MorphPad.h
    src/UI/MorphPad.cpp
    src/UI/SnapFader.h
    src/UI/SnapFader.cpp
    src/UI/SnapshotRing.h
    src/UI/SnapshotRing.cpp
    src/UI/PluginBrowserPanel.h
    src/UI/PluginBrowserPanel.cpp
    src/UI/MacroKnobStrip.h
    src/UI/MacroKnobStrip.cpp
    src/UI/AIStatusPanel.h
    src/UI/AIStatusPanel.cpp
    src/UI/BreedingPanel.h
    src/UI/BreedingPanel.cpp
    src/UI/ModeBar.h
    src/UI/ModeBar.cpp
    src/UI/HostedPluginWindow.h
    src/UI/ParameterMapPanel.h
    src/UI/ParameterMapPanel.cpp
    src/UI/BottomControlStrip.h
    src/UI/BottomControlStrip.cpp
    src/UI/V2TabBar.h
    src/UI/V2TabBar.cpp
    src/UI/EngineTabPage.h
    src/UI/EngineTabPage.cpp
    src/UI/DriftControlPanel.h
    src/UI/DriftControlPanel.cpp
    src/UI/SpectralControlPanel.h
    src/UI/SpectralControlPanel.cpp
    src/UI/GranularControlPanel.h
    src/UI/GranularControlPanel.cpp
    src/UI/HybridBlendPanel.h
    src/UI/HybridBlendPanel.cpp
    src/UI/PerformancePanel.h
    src/UI/PerformancePanel.cpp
    src/UI/ModulationMatrixPanel.h
    src/UI/ModulationMatrixPanel.cpp
    src/UI/OnboardingOverlay.h
    src/UI/OnboardingOverlay.cpp
    src/UI/V2PresetBrowserPanel.h
    src/UI/V2PresetBrowserPanel.cpp
)

target_sources(MorePhi PRIVATE
    ${MORE_PHI_PLUGIN_SOURCES}
    ${MORE_PHI_CORE_SOURCES}
    ${MORE_PHI_MASTERING_SOURCES}
    ${MORE_PHI_HOST_SOURCES}
    ${MORE_PHI_LICENSING_SOURCES}
    ${MORE_PHI_AI_SOURCES}
    ${MORE_PHI_MIDI_SOURCES}
    ${MORE_PHI_PRESET_SOURCES}
    ${MORE_PHI_UI_SOURCES}
)

# MSVC occasionally runs out of frontend heap on these large dataset translation units.
# Compile them with conservative settings to keep CI deterministic.
# /bigobj is required for large translation units; /Od removed to avoid disabling
# optimizations in Release builds. The original /Od was a conservative workaround
# that is no longer needed with current compiler settings.
if(MSVC)
    set_source_files_properties(
        src/AI/Dataset/MetadataWriter.cpp
        src/AI/Dataset/MetadataSchema.cpp
        PROPERTIES
            COMPILE_OPTIONS "/bigobj"
    )
endif()

# ── Include Paths ─────────────────────────────────────────────────────────────
target_include_directories(MorePhi PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── CLI Executable ────────────────────────────────────────────────────────────
# CLI only needs core, host, and dataset sources - NOT MCP server code
set(MORE_PHI_CLI_SOURCES
    src/CLI/main.cpp
    src/Version.cpp
    ${MORE_PHI_CORE_SOURCES}
    ${MORE_PHI_HOST_SOURCES}
    ${MORE_PHI_LICENSING_SOURCES}
    ${MORE_PHI_AI_DATASET_STABLE_SOURCES}
    ${MORE_PHI_AI_DATASET_V3_SOURCES}
)

add_executable(MorePhiCLI
    ${MORE_PHI_CLI_SOURCES}
)

set(MORE_PHI_STANDALONE_MCP_CORE_SOURCES
    src/AI/StandaloneMcp/JsonRpc.h
    src/AI/StandaloneMcp/MorePhiIPCAssistant.h
    src/AI/StandaloneMcp/MorePhiIPCAssistant.cpp
    src/AI/StandaloneMcp/MorePhiIPCDiscovery.h
    src/AI/StandaloneMcp/MorePhiIPCDiscovery.cpp
    src/AI/StandaloneMcp/MorePhiPluginBackend.h
    src/AI/StandaloneMcp/MorePhiPluginBackend.cpp
    src/AI/StandaloneMcp/StandaloneMcpServer.h
    src/AI/StandaloneMcp/StandaloneMcpServer.cpp
    ${MORE_PHI_HOST_SOURCES}
)

add_library(MorePhiMcpCore STATIC
    ${MORE_PHI_STANDALONE_MCP_CORE_SOURCES}
)

add_executable(MorePhiMcpServer
    src/AI/StandaloneMcp/StandaloneMcpMain.cpp
)

# ── CLI Configuration ─────────────────────────────────────────────────────────
target_include_directories(MorePhiCLI PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(MorePhiMcpCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_include_directories(MorePhiMcpServer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(MorePhiCLI
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_cryptography
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        nlohmann_json::nlohmann_json
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_link_libraries(MorePhiMcpCore
    PUBLIC
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_core
        juce::juce_events
        nlohmann_json::nlohmann_json
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

target_link_libraries(MorePhiMcpServer
    PRIVATE
        MorePhiMcpCore
        juce::juce_gui_basics
)

# ── HeadlessHost: GUI-initialized plugin host for headless R&D ──
add_executable(HeadlessHost
    src/Tools/HeadlessHost/HeadlessHostMain.cpp
)
target_link_libraries(HeadlessHost
    PRIVATE
        juce::juce_core
        juce::juce_events
        juce::juce_gui_basics
        juce::juce_audio_basics
        juce::juce_audio_processors
)
target_include_directories(HeadlessHost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_compile_definitions(HeadlessHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_PUSH_NOTIFICATIONS=0
)

target_compile_definitions(MorePhiCLI
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_PLUGINHOST_VST3=1
        MORE_PHI_ENABLE_DATASET_V3=1
        JUCE_PUSH_NOTIFICATIONS=0
)

target_compile_definitions(MorePhiMcpCore
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_STANDALONE_APPLICATION=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PUSH_NOTIFICATIONS=0
)

if(APPLE)
    target_compile_definitions(MorePhiCLI PUBLIC JUCE_PLUGINHOST_AU=1)
    target_compile_definitions(MorePhiMcpCore PUBLIC JUCE_PLUGINHOST_AU=1)
endif()

# ── JUCE Modules ──────────────────────────────────────────────────────────────
target_link_libraries(MorePhi
    PRIVATE
        MorePhiFonts
        juce::juce_audio_basics
        juce::juce_audio_formats
        juce::juce_audio_plugin_client
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_cryptography
        juce::juce_data_structures
        juce::juce_dsp
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra
        nlohmann_json::nlohmann_json
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(WIN32)
    target_link_libraries(MorePhi PRIVATE winhttp)
    # Crypt32: DPAPI (CryptProtectData/CryptUnprotectData) for the encrypted
    # license-cert envelope (LicenseDpapiWindows.cpp). The SharedCode lib
    # references these symbols, so every consumer (plugin + tests + CLI) needs
    # the lib on the link line.
    target_link_libraries(MorePhi PUBLIC Crypt32)
    if(TARGET MorePhiTests)
        target_link_libraries(MorePhiTests PRIVATE Crypt32)
    endif()
    if(TARGET MorePhiCLI)
        target_link_libraries(MorePhiCLI PRIVATE Crypt32)
    endif()
endif()

# ── Production Ed25519 key injection (Phase 1) ───────────────────────────────
# When MORE_PHI_PROD_ED25519_KEY_HEX is set (64 hex chars), generate a configured
# header that supplies the production public key, overriding the dev placeholder.
# The hex is validated for length=64 and [0-9a-fA-F] only; malformed input is a
# hard configure error (fail closed — never silently ship a partial/garbage key).
if(MORE_PHI_PROD_ED25519_KEY_HEX)
    set(_keyhex "${MORE_PHI_PROD_ED25519_KEY_HEX}")
    string(LENGTH "${_keyhex}" _keylen)
    string(REGEX MATCH "^[0-9a-fA-F]+$" _keyvalid "${_keyhex}")
    if(NOT (_keylen EQUAL 64 AND _keyvalid))
        message(FATAL_ERROR
            "MORE_PHI_PROD_ED25519_KEY_HEX must be exactly 64 hex chars [0-9a-fA-F]. "
            "Got length=${_keylen}. Refusing to configure with a malformed production key.")
    endif()
    string(TOLOWER "${_keyhex}" _keyhex_lower)
    configure_file(
        "${CMAKE_SOURCE_DIR}/src/Licensing/ProdSigningKey.h.in"
        "${CMAKE_BINARY_DIR}/generated/licensing/ProdSigningKey.h"
        @ONLY)
    target_include_directories(MorePhi PUBLIC "${CMAKE_BINARY_DIR}/generated")
    message(STATUS "More-Phi: production Ed25519 key injected (${_keyhex_lower}) — Release builds unblocked.")
endif()

# ── Compile Definitions ──────────────────────────────────────────────────────
target_compile_definitions(MorePhi
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_PLUGINHOST_VST3=1
        MORE_PHI_ENABLE_DATASET_V3=1
        JUCE_PUSH_NOTIFICATIONS=0   # Not used; disables WinRT pull-in on MSVC
)

# AUDIT-2026-06-25: gate optional network-exposed surfaces at compile time so
# Release builds without them compile the affected branches out entirely.
if(MORE_PHI_ENABLE_MCP_SERVER)
    target_compile_definitions(MorePhi PUBLIC MORE_PHI_ENABLE_MCP_SERVER=1)
endif()
if(MORE_PHI_ENABLE_SONICMASTER_HTTP_FALLBACK)
    target_compile_definitions(MorePhi PUBLIC MORE_PHI_ENABLE_SONICMASTER_HTTP_FALLBACK=1)
endif()

# When the test suite is enabled, mark the shared-code lib with MORE_PHI_TEST_MODE
# so the SigningKeys dev-key release-gate (an intentional ship blocker) is
# exempted for test/lib builds — real shipping Release builds (tests OFF) still
# enforce it. Without this, the test exe can't link the Release MorePhi lib.
if(MORE_PHI_BUILD_TESTS)
    target_compile_definitions(MorePhi PUBLIC MORE_PHI_TEST_MODE=1)
endif()

if(MORE_PHI_ENABLE_PROFILING)
    target_compile_definitions(MorePhi PUBLIC MORE_PHI_ENABLE_PROFILING=1)
endif()

if(APPLE)
    target_compile_definitions(MorePhi PUBLIC JUCE_PLUGINHOST_AU=1)
endif()

# ── ONNX Runtime linkage + compile definition ─────────────────────────────────
# When enabled, the runner's real session/inference path compiles in (the .cpp
# guards on MORE_PHI_HAS_ONNX) and the imported ORT target is linked PRIVATE so
# ONNX symbols do not leak into the plugin's public ABI. Stays fully off by
# default; flipping MORE_PHI_ENABLE_ONNX=ON is the single activation switch.
if(MORE_PHI_HAS_ONNX)
    target_compile_definitions(MorePhi PRIVATE MORE_PHI_HAS_ONNX=1)
    target_link_libraries(MorePhi PRIVATE
        more_phi::onnxruntime
        MorePhiSonicMasterModel
    )
    target_include_directories(MorePhi PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )

    # Copy the ONNX Runtime shared lib next to the plugin/test binaries so the
    # host process can resolve ONNX symbols at runtime (Windows DLL search path;
    # Linux/macOS rpath convenience). FetchOnnxRuntime.cmake exposes the path in
    # MORE_PHI_ONNX_RUNTIME_LIB; the copy must run POST_BUILD on every consumer
    # of the SharedCode lib that links ORT.
    if(WIN32)
        set(_ort_dll_dir "${CMAKE_CURRENT_BINARY_DIR}/_deps/onnxruntime-prebuilt/onnxruntime-win-x64-${MORE_PHI_ONNX_VERSION}/lib")
        foreach(_consumer MorePhi_VST3 MorePhi_Standalone MorePhiCLI MorePhiMcpServer HeadlessHost)
            if(TARGET ${_consumer})
                add_custom_command(TARGET ${_consumer} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_ort_dll_dir}/onnxruntime.dll"
                        "$<TARGET_FILE_DIR:${_consumer}>/onnxruntime.dll"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${_ort_dll_dir}/onnxruntime_providers_shared.dll"
                        "$<TARGET_FILE_DIR:${_consumer}>/onnxruntime_providers_shared.dll"
                    COMMENT "Copying ONNX Runtime DLLs next to ${_consumer}")
            endif()
        endforeach()
        unset(_ort_dll_dir)
        unset(_consumer)
    endif()
endif()

# ── libsodium linkage (Ed25519 license-certificate verification) ──────────────
# Ed25519Verifier.cpp uses __has_include(<sodium.h>) so it degrades gracefully if
# this is off; the compile definition just makes the include path authoritative.
if(MORE_PHI_HAS_ED25519)
    target_compile_definitions(MorePhi PRIVATE MORE_PHI_HAVE_ED25519=1)
    target_link_libraries(MorePhi PRIVATE more_phi_ed25519)
endif()

# Build-stability defaults are target-scoped so normal CMake options stay explicit.
more_phi_apply_build_tuning(MorePhi)
more_phi_apply_build_tuning(MorePhiCLI)
more_phi_apply_build_tuning(MorePhiMcpCore)
more_phi_apply_build_tuning(MorePhiMcpServer)
more_phi_apply_build_tuning(HeadlessHost)
if(TARGET MorePhi_VST3)
    more_phi_apply_build_tuning(MorePhi_VST3)
endif()
if(TARGET MorePhi_Standalone)
    more_phi_apply_build_tuning(MorePhi_Standalone)
endif()
if(TARGET MorePhi_AU)
    more_phi_apply_build_tuning(MorePhi_AU)
endif()

# Keep SIMD tuning scoped to the implementation files that use intrinsics so the
# rest of the build keeps the current compatibility envelope. Previously only
# SIMDAudio.cpp received /arch:AVX2, which meant InterpolationEngine.cpp fell
# back to the SSE2 path even on AVX-capable hosts and misreported benchmark
# results as "AVX" (see audit).
set(MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86)$")
    if(MSVC)
        set(MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS "/arch:AVX2")
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        set(MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS "-mavx2;-msse4.1")
    endif()
elseif(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # macOS Universal build (x86_64 + arm64). CMAKE_SYSTEM_PROCESSOR is the
    # host's CPU (arm64 on Apple Silicon runners), so the x86 regex above
    # doesn't match even though the build targets x86_64 as a slice. Scope
    # -mavx2/-msse4.1 to the x86_64 slice only via -Xarch; the arm64 slice
    # is unaffected (its intrinsics are #if-guarded to no-ops in source).
    set(MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS "-Xarch_x86_64;-mavx2;-Xarch_x86_64;-msse4.1")
endif()

if(MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS)
    set_source_files_properties(
        src/Core/SIMDAudio.cpp
        src/Core/InterpolationEngine.cpp
        src/Core/SpectralMorphEngine.cpp
        src/Core/GranularMorphEngine.cpp
        src/Core/FormantMorphEngine.cpp
        src/Core/EnvelopeFollower.cpp
        PROPERTIES COMPILE_OPTIONS "${MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS}"
    )
endif()

# Remove the stale standalone CompilerFlags.cmake from the build pipeline.
# Its contents reference a non-existent `morphy_interface_options` target,
# disable exceptions/RTTI (incompatible with JUCE), and set static CRT options
# that conflict with JUCE defaults. SIMD flags are now handled above.
# (The file itself is retained in cmake/ for archaeology but is not included.)

if(MORE_PHI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# T2 Phase-0: ctypes-callable headless mastering render harness. Drives the real
# AutoMasteringEngine OFFLINE so the Python CMA-ES teacher can search the 72-delta
# space against a perceptual target. OFF by default (only needed for T2 training).
option(MORE_PHI_BUILD_HEADLESS_RENDER
       "Build ctypes-callable headless mastering render harness for the T2 CMA-ES teacher"
       OFF)
if(MORE_PHI_BUILD_HEADLESS_RENDER)
    add_subdirectory(tools/headless_mastering_render)
endif()
