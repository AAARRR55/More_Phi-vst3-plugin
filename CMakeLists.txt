cmake_minimum_required(VERSION 3.24)
project(MorphSnap VERSION 3.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
option(MORPHSNAP_COPY_PLUGIN_AFTER_BUILD "Copy plugin bundle to system folder after build" OFF)
option(MORPHSNAP_BUILD_TESTS "Configure and build tests in tests/." ON)
option(MORPHSNAP_ENABLE_SANITIZERS "Enable ASAN/UBSAN in Debug builds (Clang/GCC only)" OFF)

# Windows: 4 MB stack for plugin-in-plugin hosting (FL Studio compat)
if(WIN32)
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /STACK:4194304")
endif()

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
    src/Core/AllocationTracker.h
    src/Core/ParameterClassifier.h
    src/Core/ParameterClassifier.cpp
    src/Core/DiscreteParameterHandler.h
    src/Core/DiscreteParameterHandler.cpp
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
        JUCE_PUSH_NOTIFICATIONS=0   # Not used; disables WinRT pull-in on MSVC
)

if(APPLE)
    target_compile_definitions(MorphSnap PUBLIC JUCE_PLUGINHOST_AU=1)
endif()


if(MORPHSNAP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
