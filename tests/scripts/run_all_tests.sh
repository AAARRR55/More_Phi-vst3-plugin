#!/bin/bash
# Comprehensive Test Runner for Morphy Plugin
# Runs all test suites and generates combined report

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_ROOT}/build"
PLUGIN_PATH="${BUILD_DIR}/Morphy_artefacts/Release/VST3/Morphy.vst3"
RESULTS_DIR="${PROJECT_ROOT}/test-results"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

# Create results directory
mkdir -p "${RESULTS_DIR}"

echo "=========================================="
echo "Morphy Plugin Test Suite"
echo "=========================================="
echo "Build Directory: ${BUILD_DIR}"
echo "Plugin Path: ${PLUGIN_PATH}"
echo "Results Directory: ${RESULTS_DIR}"
echo "=========================================="
echo ""

# Overall status
OVERALL_STATUS=0

# Function to print test header
print_test_header() {
    echo ""
    echo "=========================================="
    echo "$1"
    echo "=========================================="
}

# Function to check if plugin exists
check_plugin_exists() {
    if [ ! -d "${PLUGIN_PATH}" ] && [ ! -f "${PLUGIN_PATH}" ]; then
        echo -e "${RED}ERROR: Plugin not found at ${PLUGIN_PATH}${NC}"
        echo "Please build the plugin first"
        exit 1
    fi
}

# Test 1: VST3 Validation
test_vst3_validation() {
    print_test_header "Test 1: VST3 SDK Validation"

    if [ ! -f "${SCRIPT_DIR}/run_vst3_validator.py" ]; then
        echo -e "${YELLOW}WARNING: VST3 validator script not found${NC}"
        return 1
    fi

    python3 "${SCRIPT_DIR}/run_vst3_validator.py" \
        --plugin "${PLUGIN_PATH}" \
        --output "${RESULTS_DIR}/vst3_validation_${TIMESTAMP}.json" \
        --junit "${RESULTS_DIR}/vst3_validation_${TIMESTAMP}.xml" \
        || OVERALL_STATUS=1

    echo "VST3 validation complete"
}

# Test 2: Real-Time Safety
test_realtime_safety() {
    print_test_header "Test 2: Real-Time Safety Testing"

    if [ ! -f "${SCRIPT_DIR}/realtime_safety_test.py" ]; then
        echo -e "${YELLOW}WARNING: Real-time safety test script not found${NC}"
        return 1
    fi

    python3 "${SCRIPT_DIR}/realtime_safety_test.py" \
        --plugin "${PLUGIN_PATH}" \
        --duration 5 \
        --output "${RESULTS_DIR}/realtime_safety_${TIMESTAMP}.json" \
        || OVERALL_STATUS=1

    echo "Real-time safety testing complete"
}

# Test 3: Audio Quality
test_audio_quality() {
    print_test_header "Test 3: Audio Quality Testing"

    if [ ! -f "${SCRIPT_DIR}/audio_quality_test.py" ]; then
        echo -e "${YELLOW}WARNING: Audio quality test script not found${NC}"
        return 1
    fi

    python3 "${SCRIPT_DIR}/audio_quality_test.py" \
        --plugin "${PLUGIN_PATH}" \
        --output-dir "${RESULTS_DIR}/audio_quality_${TIMESTAMP}" \
        || OVERALL_STATUS=1

    echo "Audio quality testing complete"
}

# Test 4: Unit Tests
test_unit_tests() {
    print_test_header "Test 4: Unit Tests"

    if [ ! -f "${BUILD_DIR}/MorphyTests" ]; then
        echo -e "${YELLOW}WARNING: Unit test executable not found${NC}"
        return 1
    fi

    "${BUILD_DIR}/MorphyTests" \
        --use-colour yes \
        --reporter json \
        --out "${RESULTS_DIR}/unit_tests_${TIMESTAMP}.json" \
        || OVERALL_STATUS=1

    echo "Unit tests complete"
}

# Test 5: Memory Leak Detection
test_memory_leaks() {
    print_test_header "Test 5: Memory Leak Detection"

    # Run with Valgrind if available (Linux)
    if command -v valgrind &> /dev/null; then
        valgrind --leak-check=full \
            --show-leak-kinds=all \
            --track-origins=yes \
            --verbose \
            --log-file="${RESULTS_DIR}/valgrind_${TIMESTAMP}.log" \
            "${BUILD_DIR}/MorphyTests" \
            || OVERALL_STATUS=1

        echo "Memory leak detection complete"
    else
        echo -e "${YELLOW}Valgrind not available, skipping memory leak detection${NC}"
    fi
}

# Generate combined report
generate_combined_report() {
    print_test_header "Generating Combined Report"

    REPORT_FILE="${RESULTS_DIR}/combined_report_${TIMESTAMP}.json"

    cat > "${REPORT_FILE}" << EOF
{
    "timestamp": "$(date -Iseconds)",
    "plugin_path": "${PLUGIN_PATH}",
    "test_results": {
EOF

    # Add individual test results if they exist
    first=true
    for test in vst3_validation realtime_safety audio_quality unit_tests; do
        json_file=$(find "${RESULTS_DIR}" -name "${test}_*.json" | head -1)
        if [ -f "${json_file}" ]; then
            if [ "$first" = false ]; then
                echo "," >> "${REPORT_FILE}"
            fi
            echo "        \"${test}\": $(cat "${json_file}")" >> "${REPORT_FILE}"
            first=false
        fi
    done

    cat >> "${REPORT_FILE}" << EOF
    },
    "overall_status": "$( [ ${OVERALL_STATUS} -eq 0 ] && echo "PASSED" || echo "FAILED" )"
}
EOF

    echo "Combined report saved to: ${REPORT_FILE}"
}

# Print summary
print_summary() {
    print_test_header "Test Suite Summary"

    echo ""
    echo "Results Directory: ${RESULTS_DIR}"
    echo ""
    echo "Individual Test Results:"
    echo "  - VST3 Validation: ${RESULTS_DIR}/vst3_validation_${TIMESTAMP}.*"
    echo "  - Real-Time Safety: ${RESULTS_DIR}/realtime_safety_${TIMESTAMP}.json"
    echo "  - Audio Quality: ${RESULTS_DIR}/audio_quality_${TIMESTAMP}/"
    echo "  - Unit Tests: ${RESULTS_DIR}/unit_tests_${TIMESTAMP}.json"
    echo "  - Memory Leaks: ${RESULTS_DIR}/valgrind_${TIMESTAMP}.log"
    echo "  - Combined Report: ${RESULTS_DIR}/combined_report_${TIMESTAMP}.json"
    echo ""

    if [ ${OVERALL_STATUS} -eq 0 ]; then
        echo -e "${GREEN}Overall Status: PASSED${NC}"
    else
        echo -e "${RED}Overall Status: FAILED${NC}"
    fi
    echo ""
}

# Main execution
main() {
    # Check prerequisites
    check_plugin_exists

    # Run all tests
    test_vst3_validation
    test_realtime_safety
    test_audio_quality
    test_unit_tests
    test_memory_leaks

    # Generate report
    generate_combined_report

    # Print summary
    print_summary

    # Exit with appropriate code
    exit ${OVERALL_STATUS}
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --plugin-path)
            PLUGIN_PATH="$2"
            shift 2
            ;;
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --results-dir)
            RESULTS_DIR="$2"
            shift 2
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --plugin-path PATH   Path to plugin bundle"
            echo "  --build-dir PATH     Build directory"
            echo "  --results-dir PATH   Results directory"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Run main
main
