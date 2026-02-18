# CMakeLists.txt for Morphy Plugin Test Suite
# Comprehensive testing configuration for VST3/AU plugin

cmake_minimum_required(VERSION 3.20)

# Set up testing
enable_testing()

# Test configuration options
option(MORPHY_TEST_VST3_VALIDATOR "Run VST3 validator if available" ON)
option(MORPHY_TEST_AU_VALIDATOR "Run AU validator on macOS if available" ON)
option(MORPHY_TEST_MEMORY_LEAKS "Run memory leak detection tests" ON)
option(MORPHY_TEST_PERFORMANCE "Run performance benchmark tests" ON)

# Add Catch2 test framework
if(NOT TARGET Catch2::Catch2WithMain)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.4.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
endif()

# Add Google Benchmark for performance tests
if(MORPHY_TEST_PERFORMANCE)
    include(FetchContent)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(benchmark)
endif()

# Test source files
set(MORPHY_TEST_SOURCES
    # Unit tests
    test_unit_parameter_manager.cpp
    test_unit_morphing_engine.cpp
    test_unit_interpolation_curves.cpp
    test_unit_parameter_state.cpp
    test_unit_lockfree_queue.cpp

    # Integration tests
    test_integration_plugin_processor.cpp
    test_integration_mcp_client.cpp
    test_integration_ai_automation.cpp

    # Real-time safety tests
    test_rt_memory_allocation.cpp
    test_rt_lock_contention.cpp
    test_rt_cpu_spikes.cpp
    test_rt_audio_thread_violations.cpp

    # Audio quality tests
    test_audio_parameter_smoothing.cpp
    test_audio_click_pop_detection.cpp
    test_audio_sample_accuracy.cpp
    test_audio_crossfade_quality.cpp
)

# Create test executable
add_executable(MorphyTests ${MORPHY_TEST_SOURCES})

# Link against plugin sources (for testing)
target_link_libraries(MorphyTests PRIVATE
    Catch2::Catch2WithMain
    benchmark::benchmark
    benchmark::benchmark_main
)

# Set up include directories
target_include_directories(MorphyTests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/src
)

# Set compile definitions for testing
target_compile_definitions(MorphyTests PRIVATE
    MORPHY_TEST_MODE=1
    JUCE_UNIT_TESTS=1
)

# Platform-specific test configurations
if(WIN32)
    target_compile_definitions(MorphyTests PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )

    # Add Windows-specific tests
    target_sources(MorphyTests PRIVATE
        test_platform_windows.cpp
    )

    # Link against Windows libraries
    target_link_libraries(MorphyTests PRIVATE
        winmm
    )
elseif(APPLE)
    # Add macOS-specific tests
    target_sources(MorphyTests PRIVATE
        test_platform_macos.cpp
    )

    # Link against macOS frameworks
    target_link_libraries(MorphyTests PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
    )
endif()

# Add test discovery
include(CTest)

# Add tests to CTest
add_test(NAME UnitTests COMMAND MorphyTests)

# Memory leak detection with Valgrind (Linux)
if(MORPHY_TEST_MEMORY_LEAKS AND UNIX AND NOT APPLE)
    find_program(VALGRIND_EXECUTABLE valgrind)
    if(VALGRIND_EXECUTABLE)
        add_test(NAME MemoryLeakDetection
            COMMAND ${VALGRIND_EXECUTABLE}
                --leak-check=full
                --show-leak-kinds=all
                --track-origins=yes
                --error-exitcode=1
                $<TARGET_FILE:MorphyTests>
        )
        set_tests_properties(MemoryLeakDetection PROPERTIES
            TIMEOUT 300
            LABELS "memory"
        )
    endif()
endif()

# VST3 Validator test
if(MORPHY_TEST_VST3_VALIDATOR)
    find_program(VST3_VALIDATOR vst3_validator)
    if(VST3_VALIDATOR)
        add_test(NAME VST3Validation
            COMMAND ${VST3_VALIDATOR} -r $<TARGET_FILE:Morphy>
        )
        set_tests_properties(VST3Validation PROPERTIES
            TIMEOUT 120
            LABELS "validation"
        )
    endif()
endif()

# AU Validator test (macOS only)
if(MORPHY_TEST_AU_VALIDATOR AND APPLE)
    find_program(AUVAL auval)
    if(AUVAL)
        add_test(NAME AUValidation
            COMMAND ${AUVAL} -v aufx Mrph MPAU
        )
        set_tests_properties(AUValidation PROPERTIES
            TIMEOUT 120
            LABELS "validation"
        )
    endif()
endif()

# Performance benchmarks
if(MORPHY_TEST_PERFORMANCE)
    add_test(NAME PerformanceBenchmarks
        COMMAND MorphyTests --benchmark
    )
    set_tests_properties(PerformanceBenchmarks PROPERTIES
        TIMEOUT 600
        LABELS "performance"
    )
endif()

# Custom test target for running all tests
add_custom_target(run_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS MorphyTests
    COMMENT "Running all tests..."
)

# Custom target for running tests with Valgrind
add_custom_target(run_memory_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L memory --output-on-failure
    DEPENDS MorphyTests
    COMMENT "Running memory leak tests..."
)

# Custom target for running validation tests
add_custom_target(run_validation_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L validation --output-on-failure
    DEPENDS MorphyTests
    COMMENT "Running validation tests..."
)

# Custom target for running performance tests
add_custom_target(run_performance_tests
    COMMAND ${CMAKE_CTEST_COMMAND} -L performance --output-on-failure
    DEPENDS MorphyTests
    COMMENT "Running performance benchmarks..."
)

# Install test scripts
install(FILES
    scripts/run_vst3_validator.py
    scripts/realtime_safety_test.py
    scripts/audio_quality_test.py
    scripts/run_all_tests.sh
    scripts/run_all_tests.ps1
    DESTINATION bin
    PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                GROUP_READ GROUP_EXECUTE
                WORLD_READ WORLD_EXECUTE
)

# Install Python test dependencies
install(CODE "
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo
        \"Installing Python test dependencies...\"
    )
    # Note: Dependencies should be installed via pip
    # pip install numpy soundfile
")

# Print test configuration summary
message(STATUS "")
message(STATUS "==================================")
message(STATUS "Morphy Test Suite Configuration")
message(STATUS "==================================")
message(STATUS "VST3 Validator: ${VST3_VALIDATOR}")
message(STATUS "AU Validator: ${AUVAL}")
message(STATUS "Valgrind: ${VALGRIND_EXECUTABLE}")
message(STATUS "Memory Tests: ${MORPHY_TEST_MEMORY_LEAKS}")
message(STATUS "Performance Tests: ${MORPHY_TEST_PERFORMANCE}")
message(STATUS "==================================")
message(STATUS "")
