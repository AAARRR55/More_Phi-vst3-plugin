cmake_minimum_required(VERSION 3.24)
project(MorphSnap VERSION 3.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(MORPHSNAP_COPY_PLUGIN_AFTER_BUILD "Copy plugin bundle to system folder after build" OFF)
option(MORPHSNAP_BUILD_TESTS "Configure and build tests in tests/." ON)
option(MORPHSNAP_ENABLE_SANITIZERS "Enable ASAN/UBSAN in Debug builds (Clang/GCC only)" OFF)
option(MORPHSNAP_SAFE_BUILD_MODE "Enable conservative build settings for local stability" ON)
set(MORPHSNAP_MSVC_MP "2" CACHE STRING "MSVC /MP process count (set 0 to disable /MP).")
option(MORPHSNAP_ENABLE_LTO "Enable LTO for Release builds" OFF)
option(MORPHSNAP_ENABLE_DATASET_V3 "Deprecated compatibility flag (no-op): Dataset V3 modular pipeline sources are always compiled" OFF)

# Keep single-config generators deterministic when callers omit build type.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

if(MSVC AND NOT MORPHSNAP_MSVC_MP MATCHES "^[0-9]+$")
    message(FATAL_ERROR "MORPHSNAP_MSVC_MP must be numeric (for example: 0, 1, 2, 4).")
endif()

# Windows: 4 MB stack for plugin-in-plugin hosting (FL Studio compat)
if(WIN32)
    if(MSVC)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /STACK:4194304")
    elseif(MINGW)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--stack,4194304")
    endif()
endif()

function(morphsnap_apply_build_tuning target_name)
    if(NOT TARGET ${target_name})
        message(FATAL_ERROR "Cannot tune build target '${target_name}' because it does not exist.")
    endif()

    get_target_property(_morphsnap_target_type ${target_name} TYPE)
    if(_morphsnap_target_type STREQUAL "INTERFACE_LIBRARY" OR _morphsnap_target_type STREQUAL "UTILITY")
        return()
    endif()

    if(MSVC)
        # Keep PCH memory and CL internal workers explicit so local machines can tune safely.
        target_compile_options(${target_name} PRIVATE /Zm200)
        if(NOT MORPHSNAP_MSVC_MP STREQUAL "0")
            target_compile_options(${target_name} PRIVATE "/MP${MORPHSNAP_MSVC_MP}")
        endif()

        if(MORPHSNAP_ENABLE_LTO)
            target_compile_options(${target_name} PRIVATE $<$<CONFIG:Release>:/GL>)
        endif()

        if(
            _morphsnap_target_type STREQUAL "EXECUTABLE"
            OR _morphsnap_target_type STREQUAL "SHARED_LIBRARY"
            OR _morphsnap_target_type STREQUAL "MODULE_LIBRARY"
        )
            if(MORPHSNAP_SAFE_BUILD_MODE)
                target_link_options(${target_name} PRIVATE /cgthreads:2)
            endif()

            if(MORPHSNAP_ENABLE_LTO)
                target_link_options(${target_name} PRIVATE $<$<CONFIG:Release>:/LTCG>)
            endif()
        endif()
    elseif(MORPHSNAP_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT morphsnap_ipo_supported OUTPUT morphsnap_ipo_output)
        if(morphsnap_ipo_supported)
            set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        else()
            message(WARNING "LTO requested but IPO is not supported for ${target_name}: ${morphsnap_ipo_output}")
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

# Patch JUCE headers that conflict with Windows macros on MSVC
include(cmake/PatchJuceForMSVC.cmake)


# Plugin formats: AU is macOS-only, do not emit it on other platforms
if(APPLE)
    set(MORPHSNAP_PLUGIN_FORMATS VST3 AU)
else()
    set(MORPHSNAP_PLUGIN_FORMATS VST3)
endif()

# ── Plugin Target ─────────────────────────────────────────────────────────────
juce_add_plugin(MorphSnap
    COMPANY_NAME            "MorphSnap"
    PLUGIN_MANUFACTURER_CODE Mrph
    PLUGIN_CODE              Mrps
    FORMATS                  ${MORPHSNAP_PLUGIN_FORMATS}
    PRODUCT_NAME             "MorphSnap"
    IS_SYNTH                 FALSE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        FALSE
    IS_MIDI_EFFECT           FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD  ${MORPHSNAP_COPY_PLUGIN_AFTER_BUILD}
    VST3_CATEGORIES          "Fx" "Tools"
)

# ── Source Files ──────────────────────────────────────────────────────────────
# Phase 1: Foundation
set(MORPHSNAP_PLUGIN_SOURCES
    src/Plugin/PluginProcessor.h
    src/Plugin/PluginProcessor.cpp
    src/Plugin/PluginEditor.h
    src/Plugin/PluginEditor.cpp
)

set(MORPHSNAP_CORE_SOURCES
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
    src/Core/LockFreeQueue.h
    src/Core/ThreadPool.h
    src/Core/ThreadPool.cpp
    src/Core/AllocationTracker.h
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
    src/Core/IModulationEngine.h
    src/Core/ModulationEngine.h
    src/Core/ModulationEngine.cpp
    src/Core/ISpectralMorphEngine.h
    src/Core/IGranularMorphEngine.h
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
    src/Core/VAEMorphEngine.h
    src/Core/VAEMorphEngine.cpp
)

set(MORPHSNAP_HOST_SOURCES
    src/Host/PluginScanner.h
    src/Host/PluginScanner.cpp
    src/Host/IPluginHostManager.h
    src/Host/PluginHostManager.h
    src/Host/PluginHostManager.cpp
    src/Host/ParameterBridge.h
    src/Host/ParameterBridge.cpp
)

set(MORPHSNAP_AI_DATASET_STABLE_SOURCES
    src/AI/Dataset/DatasetGenerator.h
    src/AI/Dataset/DatasetGenerator.cpp
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
    src/AI/Dataset/ValidationEngine.h
    src/AI/Dataset/ValidationEngine.cpp
    src/AI/Dataset/DatasetOrganizer.h
    src/AI/Dataset/DatasetOrganizer.cpp
    src/AI/Dataset/DatasetGeneratorV2.h
    src/AI/Dataset/DatasetGeneratorV2.cpp
    src/AI/Dataset/DatasetConfig.h
)

set(MORPHSNAP_AI_DATASET_V3_SOURCES
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

set(MORPHSNAP_AI_SOURCES
    src/AI/InstanceIdentity.h
    src/AI/InstanceRegistry.h
    src/AI/InstanceRegistry.cpp
    src/AI/MCPServer.h
    src/AI/MCPServer.cpp
    src/AI/MCPToolHandler.h
    src/AI/MCPToolHandler.cpp
    src/AI/LinkBroadcaster.h
    src/AI/LinkBroadcaster.cpp
    src/AI/TokenOptimizer.h
    src/AI/TokenOptimizer.cpp
    src/AI/MCPToolsExtended.h
    src/AI/MCPToolsExtended.cpp
    ${MORPHSNAP_AI_DATASET_STABLE_SOURCES}
    ${MORPHSNAP_AI_DATASET_V3_SOURCES}
)

set(MORPHSNAP_MIDI_SOURCES
    src/MIDI/MIDIRouter.h
    src/MIDI/MIDIRouter.cpp
)

set(MORPHSNAP_PRESET_SOURCES
    src/Preset/MetaPresetManager.h
    src/Preset/MetaPresetManager.cpp
    src/Preset/PresetSerializer.h
    src/Preset/PresetSerializer.cpp
    src/Preset/PresetEntry.h
    src/Preset/PresetSerializerV2.h
    src/Preset/PresetSerializerV2.cpp
    src/Preset/PresetLibrary.h
    src/Preset/PresetLibrary.cpp
    src/Preset/CloudSyncClient.h
    src/Preset/CloudSyncClient.cpp
)

set(MORPHSNAP_UI_SOURCES
    src/UI/MorphSnapLookAndFeel.h
    src/UI/MorphSnapLookAndFeel.cpp
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
    src/UI/HostedPluginWindow.cpp
    src/UI/ParameterMapPanel.h
    src/UI/ParameterMapPanel.cpp
    src/UI/BottomControlStrip.h
    src/UI/BottomControlStrip.cpp
    src/UI/LearnModePanel.h
    src/UI/LearnModePanel.cpp
    src/UI/V2TabBar.h
    src/UI/V2TabBar.cpp
    src/UI/EngineTabPage.h
    src/UI/EngineTabPage.cpp
    src/UI/SpectralControlPanel.h
    src/UI/SpectralControlPanel.cpp
    src/UI/GranularControlPanel.h
    src/UI/GranularControlPanel.cpp
    src/UI/HybridBlendPanel.h
    src/UI/HybridBlendPanel.cpp
    src/UI/ModulationMatrixPanel.h
    src/UI/ModulationMatrixPanel.cpp
    src/UI/V2PresetBrowserPanel.h
    src/UI/V2PresetBrowserPanel.cpp
)

target_sources(MorphSnap PRIVATE
    ${MORPHSNAP_PLUGIN_SOURCES}
    ${MORPHSNAP_CORE_SOURCES}
    ${MORPHSNAP_HOST_SOURCES}
    ${MORPHSNAP_AI_SOURCES}
    ${MORPHSNAP_MIDI_SOURCES}
    ${MORPHSNAP_PRESET_SOURCES}
    ${MORPHSNAP_UI_SOURCES}
)

# MSVC occasionally runs out of frontend heap on this large TU in Release builds.
# Compile it with conservative settings to keep CI deterministic.
if(MSVC)
    set_source_files_properties(
        src/AI/Dataset/MetadataWriter.cpp
        PROPERTIES
            COMPILE_OPTIONS "/bigobj;/Od"
    )
endif()

# ── Include Paths ─────────────────────────────────────────────────────────────
target_include_directories(MorphSnap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ── JUCE Modules ──────────────────────────────────────────────────────────────
target_link_libraries(MorphSnap
    PRIVATE
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

# ── Compile Definitions ──────────────────────────────────────────────────────
target_compile_definitions(MorphSnap
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_PLUGINHOST_VST3=1
        MORPHSNAP_ENABLE_DATASET_V3=1
        JUCE_PUSH_NOTIFICATIONS=0   # Not used; disables WinRT pull-in on MSVC
)

if(APPLE)
    target_compile_definitions(MorphSnap PUBLIC JUCE_PLUGINHOST_AU=1)
endif()

# Build-stability defaults are target-scoped so normal CMake options stay explicit.
morphsnap_apply_build_tuning(MorphSnap)
if(TARGET MorphSnap_VST3)
    morphsnap_apply_build_tuning(MorphSnap_VST3)
endif()
if(TARGET MorphSnap_AU)
    morphsnap_apply_build_tuning(MorphSnap_AU)
endif()

if(MORPHSNAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
