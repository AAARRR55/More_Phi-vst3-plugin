# =============================================================================
# tools/headless_mastering_render/CMakeLists.txt
#
# T2 Phase-0 headless mastering render harness — ctypes-callable .so that
# drives the REAL more_phi::AutoMasteringEngine OFFLINE.
#
# Wired into the top-level CMakeLists.txt via:
#   option(MORE_PHI_BUILD_HEADLESS_RENDER "Build ctypes-callable headless
#          mastering render harness" OFF)
#   if(MORE_PHI_BUILD_HEADLESS_RENDER)
#       add_subdirectory(tools/headless_mastering_render)
#   endif()
# (add near line 754, next to add_subdirectory(tests)).
#
# Reuses MORE_PHI_MASTERING_SOURCES + MORE_PHI_CORE_SOURCES (defined at
# CMakeLists.txt:205-295) so it links the EXACT same source set the plugin
# compiles — no parity drift.
# =============================================================================

# MORE_PHI_*_SOURCES entries are repo-root-relative; make them absolute so they
# resolve correctly from this subdirectory (otherwise CMake looks under tools/.../src/).
# AutoMasteringEngine owns EQParameterTranslator/GenreClassifier/ChainPlanExecutor
# by VALUE (AutoMasteringEngine.h:161-163) and ChainPlanExecutor pulls Ozone deps,
# so those .cpp's must be linked. Added explicitly -- NOT the whole
# MORE_PHI_AI_SOURCES (which drags in MCP/LLM/TCP/dataset bloat).
set(_harness_ai_deps
    src/AI/EQParameterTranslator.cpp
    src/AI/GenreClassifier.cpp
    src/AI/ChainPlanExecutor.cpp)
# OzonePlanApplicator/OzoneParameterMap are intentionally EXCLUDED: they are
# plugin-coupled (OzonePlanApplicator ctor takes a MorePhiProcessor& and calls
# enqueueParameterSet -> pulls the whole plugin into the link). ChainPlanExecutor
# only holds an OzonePlanApplicatorBase* (abstract interface) whose vtable
# resolves via vague linkage from the inline-defaulted dtor -- no concrete Ozone
# .cpp is needed, and the harness never wires Ozone (prepare(false)).
set(_harness_srcs ${MORE_PHI_MASTERING_SOURCES} ${MORE_PHI_CORE_SOURCES} ${_harness_ai_deps})
list(TRANSFORM _harness_srcs PREPEND "${CMAKE_SOURCE_DIR}/")

add_library(more_phi_headless_render MODULE
    headless_render.cpp
    headless_render.h
    ${_harness_srcs}                   # AutoMasteringEngine + 11 DSP stages
                                       #   + NeuralMasteringSafetyPolicy
                                       #   + OnnxNeuralMasteringRunner (ONNX-free)
                                       #   + Core deps (AudioBufferPool/SIMDAudio/etc.)
)

# Apply the project's scoped SIMD flag (-mavx2 -msse4.1 on GCC/Clang x86_64) to
# the intrinsic-using files. The root CMakeLists sets this on RELATIVE paths,
# which don't match the ABSOLUTE paths the harness compiles (after the PREPEND
# above) -- re-apply on the absolute paths so AVX2 intrinsics compile.
if(MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS)
    set(_harness_simd_files
        src/Core/SIMDAudio.cpp
        src/Core/InterpolationEngine.cpp
        src/Core/SpectralMorphEngine.cpp
        src/Core/GranularMorphEngine.cpp
        src/Core/FormantMorphEngine.cpp
        src/Core/EnvelopeFollower.cpp)
    list(TRANSFORM _harness_simd_files PREPEND "${CMAKE_SOURCE_DIR}/")
    set_source_files_properties(${_harness_simd_files}
        PROPERTIES COMPILE_OPTIONS "${MORE_PHI_SIMD_SOURCE_COMPILE_OPTIONS}")
endif()

target_include_directories(more_phi_headless_render PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# JUCE module set mirrors what AutoMasteringEngine.h transitively requires:
#   juce_audio_basics : AudioBuffer, MathConstants
#   juce_core         : general utilities
#   juce_events       : Timer base class of AutoMasteringEngine
#   juce_dsp          : oversampling / DSP helpers used by stage TUs
#   juce_gui_basics   : ScopedJuceInitialiser_GUI (the one gui pull-in; the
#                       harness has no main(), so this inits the MessageManager
#                       the Timer base references even when stopped)
# nlohmann_json is pulled in transitively by EQParameterTranslator.h /
# ChainPlanExecutor.h / GenreClassifier.h (included by AutoMasteringEngine.h).
target_link_libraries(more_phi_headless_render PRIVATE
    juce::juce_audio_basics
    juce::juce_core
    juce::juce_events
    juce::juce_dsp
    juce::juce_gui_basics
    nlohmann_json::nlohmann_json
)

set_target_properties(more_phi_headless_render PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    # Ensure the render symbol is exported undecorated for ctypes dlopen.
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

target_compile_definitions(more_phi_headless_render PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PUSH_NOTIFICATIONS=0
    JUCE_STANDALONE_APPLICATION=1     # avoid pulling plugin-client code
    JUCE_PLUGINHOST_VST3=0            # harness hosts no plugins
    MORE_PHI_HAS_ONNX=0               # buildPlanCandidate is ONNX-free; never
                                      #   download the prebuilt ORT binary
)

# Windows stack + Linux .so naming.
if(MSVC)
    target_link_options(more_phi_headless_render PRIVATE /STACK:4194304)
endif()

# Output a predictable library name on every platform:
#   Linux:   libmore_phi_headless_render.so
#   macOS:   libmore_phi_headless_render.dylib
#   Windows: more_phi_headless_render.dll
set_target_properties(more_phi_headless_render PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME "more_phi_headless_render"
)

# Apply the project's standard MSVC tuning (/utf-8, /MP, /Zm200, Undef min/max).
if(COMMAND more_phi_apply_build_tuning)
    more_phi_apply_build_tuning(more_phi_headless_render)
endif()

# =============================================================================
# Top-level CMakeLists.txt addition (paste near line 754, after tests):
# =============================================================================
# option(MORE_PHI_BUILD_HEADLESS_RENDER
#        "Build ctypes-callable headless mastering render harness for the T2 CMA-ES teacher"
#        OFF)
# if(MORE_PHI_BUILD_HEADLESS_RENDER)
#     add_subdirectory(tools/headless_mastering_render)
# endif()
