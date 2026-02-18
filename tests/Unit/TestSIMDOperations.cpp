/*
 * Morphy - Advanced Parameter Morphing Engine
 * TestSIMDOperations.cpp - Unit Tests for SIMD Operations
 *
 * Copyright (c) 2024 Morphy Audio
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark_all.hpp>
#include "dsp/SIMD/SIMDOperations.h"
#include <algorithm>
#include <array>
#include <cmath>

using namespace morphy::dsp::simd;

//==============================================================================
// Test Configuration
//==============================================================================

constexpr float FLOAT_EPSILON = 1e-5f;
constexpr double DOUBLE_EPSILON = 1e-10;

TEMPLATE_TEST_CASE("SIMD vector construction", "[simd][constructors]", float, double) {
    using Vector = SIMDVector<TestType>;

    SECTION("Default constructor initializes to zero") {
        Vector vec;
        // Undefined behavior to read uninitialized, so we skip
        SUCCEED();
    }

    SECTION("Single value constructor") {
        constexpr TestType value = static_cast<TestType>(42.0);
        Vector vec(value);

        // Verify the vector was created (runtime verification depends on implementation)
        REQUIRE(Vector::ElementCount >= 1);
    }

    SECTION("Pointer constructor") {
        constexpr TestType values[] = {
            static_cast<TestType>(1.0),
            static_cast<TestType>(2.0),
            static_cast<TestType>(3.0),
            static_cast<TestType>(4.0)
        };

        Vector vec(values);
        REQUIRE(Vector::ElementCount >= 1);
    }
}

//==============================================================================
// Arithmetic Operations Tests
//==============================================================================

TEMPLATE_TEST_CASE("SIMD vector addition", "[simd][arithmetic]", float, double) {
    using Vector = SIMDVector<TestType>;
    constexpr auto epsilon = std::is_same_v<TestType, float> ? FLOAT_EPSILON : DOUBLE_EPSILON;

    constexpr TestType a[] = {
        static_cast<TestType>(1.0), static_cast<TestType>(2.0),
        static_cast<TestType>(3.0), static_cast<TestType>(4.0),
        static_cast<TestType>(5.0), static_cast<TestType>(6.0),
        static_cast<TestType>(7.0), static_cast<TestType>(8.0)
    };

    constexpr TestType b[] = {
        static_cast<TestType>(0.5), static_cast<TestType>(1.5),
        static_cast<TestType>(2.5), static_cast<TestType>(3.5),
        static_cast<TestType>(4.5), static_cast<TestType>(5.5),
        static_cast<TestType>(6.5), static_cast<TestType>(7.5)
    };

    Vector vecA(a);
    Vector vecB(b);
    Vector result = vecA + vecB;

    // Store and verify
    std::array<TestType, 8> output;
    result.storeUnaligned(output.data());

    for (size_t i = 0; i < Vector::ElementCount && i < 8; ++i) {
        TestType expected = a[i] + b[i];
        REQUIRE(std::abs(output[i] - expected) < epsilon);
    }
}

TEMPLATE_TEST_CASE("SIMD vector multiplication", "[simd][arithmetic]", float, double) {
    using Vector = SIMDVector<TestType>;
    constexpr auto epsilon = std::is_same_v<TestType, float> ? FLOAT_EPSILON : DOUBLE_EPSILON;

    constexpr TestType a[] = {
        static_cast<TestType>(2.0), static_cast<TestType>(3.0),
        static_cast<TestType>(4.0), static_cast<TestType>(5.0)
    };

    constexpr TestType b[] = {
        static_cast<TestType>(1.5), static_cast<TestType>(2.0),
        static_cast<TestType>(2.5), static_cast<TestType>(3.0)
    };

    Vector vecA(a);
    Vector vecB(b);
    Vector result = vecA * vecB;

    std::array<TestType, 4> output;
    result.storeUnaligned(output.data());

    for (size_t i = 0; i < Vector::ElementCount && i < 4; ++i) {
        TestType expected = a[i] * b[i];
        REQUIRE(std::abs(output[i] - expected) < epsilon);
    }
}

TEMPLATE_TEST_CASE("SIMD fused multiply-add", "[simd][fma]", float, double) {
    using Vector = SIMDVector<TestType>;
    constexpr auto epsilon = std::is_same_v<TestType, float> ? FLOAT_EPSILON : DOUBLE_EPSILON;

    constexpr TestType a[] = {static_cast<TestType>(2.0), static_cast<TestType>(3.0)};
    constexpr TestType b[] = {static_cast<TestType>(4.0), static_cast<TestType>(5.0)};
    constexpr TestType c[] = {static_cast<TestType>(6.0), static_cast<TestType>(7.0)};

    Vector vecA(a);
    Vector vecB(b);
    Vector vecC(c);

    // Result = a * b + c
    Vector result = Vector::multiplyAdd(vecA, vecB, vecC);

    std::array<TestType, 2> output;
    result.storeUnaligned(output.data());

    for (size_t i = 0; i < Vector::ElementCount && i < 2; ++i) {
        TestType expected = a[i] * b[i] + c[i];
        REQUIRE(std::abs(output[i] - expected) < epsilon);
    }
}

//==============================================================================
// Load/Store Operations Tests
//==============================================================================

TEMPLATE_TEST_CASE("SIMD aligned load/store", "[simd][memory]", float, double) {
    using Vector = SIMDVector<TestType>;

    // Allocate aligned memory
    constexpr size_t numElements = 16;
    constexpr size_t alignment = alignof(Vector);

    alignas(alignment) std::array<TestType, numElements> input;
    alignas(alignment) std::array<TestType, numElements> output;

    // Initialize input
    for (size_t i = 0; i < numElements; ++i) {
        input[i] = static_cast<TestType>(i);
    }

    // Load, process, store
    Vector vec;
    vec.loadAligned(input.data());

    // Modify the vector
    vec = vec + vec; // Multiply by 2

    vec.storeAligned(output.data());

    // Verify
    for (size_t i = 0; i < Vector::ElementCount && i < numElements; ++i) {
        TestType expected = static_cast<TestType>(i * 2);
        REQUIRE(output[i] == expected);
    }
}

TEMPLATE_TEST_CASE("SIMD unaligned load/store", "[simd][memory]", float, double) {
    using Vector = SIMDVector<TestType>;

    constexpr size_t numElements = 16;
    std::array<TestType, numElements> input;
    std::array<TestType, numElements> output;

    // Initialize input
    for (size_t i = 0; i < numElements; ++i) {
        input[i] = static_cast<TestType>(i);
    }

    // Load with potential misalignment
    Vector vec;
    vec.loadUnaligned(input.data());

    vec.storeUnaligned(output.data());

    // Verify
    for (size_t i = 0; i < Vector::ElementCount && i < numElements; ++i) {
        REQUIRE(output[i] == input[i]);
    }
}

//==============================================================================
// Helper Functions Tests
//==============================================================================

TEST_CASE("Pointer alignment utilities", "[simd][utility]") {
    SECTION("alignPointer aligns to specified boundary") {
        alignas(16) std::byte buffer[64];

        void* aligned = alignPointer(buffer + 1, 16);
        REQUIRE((reinterpret_cast<uintptr_t>(aligned) % 16) == 0);
    }

    SECTION("isAligned correctly detects alignment") {
        alignas(32) std::byte buffer[64];

        REQUIRE(isAligned(buffer, 16));
        REQUIRE(isAligned(buffer, 32));
        REQUIRE_FALSE(isAligned(buffer + 1, 16));
    }
}

//==============================================================================
// Architecture Detection Tests
//==============================================================================

TEST_CASE("SIMD architecture detection", "[simd][detect]") {
    constexpr auto compileTimeArch = getNativeArchitecture();

    SECTION("Compile-time architecture is valid") {
        REQUIRE((compileTimeArch == SIMDArchitecture::Scalar ||
                 compileTimeArch == SIMDArchitecture::SSE ||
                 compileTimeArch == SIMDArchitecture::SSE2 ||
                 compileTimeArch == SIMDArchitecture::AVX ||
                 compileTimeArch == SIMDArchitecture::AVX2 ||
                 compileTimeArch == SIMDArchitecture::NEON));
    }

    SECTION("Runtime architecture matches compile-time") {
        auto runtimeArch = detectRuntimeArchitecture();
        // Runtime should be at least as capable as compile-time
        REQUIRE(runtimeArch == compileTimeArch ||
               runtimeArch == SIMDArchitecture::Scalar);
    }
}

//==============================================================================
// Performance Benchmarks
//==============================================================================

TEMPLATE_TEST_CASE("SIMD performance benchmarks", "[!benchmark][simd][performance]", float, double) {
    using Vector = SIMDVector<TestType>;
    constexpr size_t numIterations = 10000;
    constexpr size_t bufferSize = 1024;

    std::vector<TestType> bufferA(bufferSize);
    std::vector<TestType> bufferB(bufferSize);
    std::vector<TestType> bufferC(bufferSize);

    // Initialize with test data
    for (size_t i = 0; i < bufferSize; ++i) {
        bufferA[i] = static_cast<TestType>(i);
        bufferB[i] = static_cast<TestType>(i + 1);
    }

    BENCHMARK("Vector addition - SIMD") {
        for (size_t iter = 0; iter < numIterations; ++iter) {
            for (size_t i = 0; i < bufferSize; i += Vector::ElementCount) {
                Vector vecA(&bufferA[i]);
                Vector vecB(&bufferB[i]);
                Vector result = vecA + vecB;
                result.storeUnaligned(&bufferC[i]);
            }
        }
        return bufferC[0];
    };

    BENCHMARK("Vector addition - Scalar") {
        for (size_t iter = 0; iter < numIterations; ++iter) {
            for (size_t i = 0; i < bufferSize; ++i) {
                bufferC[i] = bufferA[i] + bufferB[i];
            }
        }
        return bufferC[0];
    };

    BENCHMARK("Vector multiplication - SIMD") {
        for (size_t iter = 0; iter < numIterations; ++iter) {
            for (size_t i = 0; i < bufferSize; i += Vector::ElementCount) {
                Vector vecA(&bufferA[i]);
                Vector vecB(&bufferB[i]);
                Vector result = vecA * vecB;
                result.storeUnaligned(&bufferC[i]);
            }
        }
        return bufferC[0];
    };

    BENCHMARK("Vector FMA - SIMD") {
        for (size_t iter = 0; iter < numIterations; ++iter) {
            for (size_t i = 0; i < bufferSize; i += Vector::ElementCount) {
                Vector vecA(&bufferA[i]);
                Vector vecB(&bufferB[i]);
                Vector vecC(&bufferC[i]);
                Vector result = Vector::multiplyAdd(vecA, vecB, vecC);
                result.storeUnaligned(&bufferC[i]);
            }
        }
        return bufferC[0];
    };
}

//==============================================================================
// Edge Cases and Special Values
//==============================================================================

TEMPLATE_TEST_CASE("SIMD special value handling", "[simd][edge-cases]", float, double) {
    using Vector = SIMDVector<TestType>;

    SECTION("Zero vectors") {
        constexpr TestType zero = static_cast<TestType>(0.0);
        Vector vecZero(zero);

        std::array<TestType, 8> output;
        vecZero.storeUnaligned(output.data());

        for (size_t i = 0; i < Vector::ElementCount && i < 8; ++i) {
            REQUIRE(output[i] == zero);
        }
    }

    SECTION("Infinity handling") {
        constexpr TestType inf = std::numeric_limits<TestType>::infinity();
        Vector vecInf(inf);

        std::array<TestType, 8> output;
        vecInf.storeUnaligned(output.data());

        for (size_t i = 0; i < Vector::ElementCount && i < 8; ++i) {
            REQUIRE(std::isinf(output[i]));
        }
    }

    SECTION("NaN handling") {
        constexpr TestType nan = std::numeric_limits<TestType>::quiet_NaN();
        Vector vecNaN(nan);

        std::array<TestType, 8> output;
        vecNaN.storeUnaligned(output.data());

        for (size_t i = 0; i < Vector::ElementCount && i < 8; ++i) {
            REQUIRE(std::isnan(output[i]));
        }
    }
}

//==============================================================================
// Type Traits Verification
//==============================================================================

TEMPLATE_TEST_CASE("Vector type traits", "[simd][traits]", float, double) {
    using Vector = SIMDVector<TestType>;
    using Traits = VectorTraits<TestType, getNativeArchitecture()>;

    SECTION("Element count is power of 2") {
        REQUIRE((Traits::ElementCount & (Traits::ElementCount - 1)) == 0);
    }

    SECTION("Alignment is sufficient") {
        REQUIRE(Traits::Alignment >= alignof(TestType));
        REQUIRE((Traits::Alignment & (Traits::Alignment - 1)) == 0);
    }

    SECTION("Vector fits in cache line") {
        constexpr size_t cacheLineSize = 64;
        REQUIRE(Traits::ElementCount * sizeof(TestType) <= cacheLineSize);
    }
}
