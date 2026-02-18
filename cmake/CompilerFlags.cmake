# cmake/CompilerFlags.cmake
# Compiler-specific flags for Morphy plugin

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "MSVC|GNU|Clang")
    message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# ============================================================================
# Common Warning Flags
# ============================================================================

set(MORPHY_COMMON_WARNINGS
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
    -Werror=non-virtual-dtor
    -Wshadow
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wconversion
    -Wsign-conversion
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wnull-dereference
    -Wuseless-cast
    -Wdouble-promotion
    -Wformat=2
    -Wlifetime
)

# ============================================================================
# MSVC Flags
# ============================================================================

if(MSVC)
    # Warning level 4 with warnings as errors
    target_compile_options(morphy_interface_options
        INTERFACE
        /W4
        /WX
        /permissive-
        /w14640
        /w14242
        /w14254
        /w14263
        /w14265
        /w14287
        /we4289
        /w14296
        /w14311
        /w14545
        /w14546
        /w14547
        /w14549
        /w14555
        /w14619
        /w14640
        /w14826
        /w14905
        /w14906
        /w14928
    )

    # Release optimizations
    set(MORPHY_RELEASE_FLAGS
        /O2
        /GL
        /Gy
        /arch:AVX2
        /DNDEBUG
    )

    # Debug flags
    set(MORPHY_DEBUG_FLAGS
        /Od
        /RTC1
        /Zi
    )

    # Enable parallel compilation
    add_compile_options(/MP)

    # Treat warnings as errors in release builds
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(/WX)
    endif()

    # Disable specific warnings for JUCE compatibility
    add_compile_options(
        /wd4996 # Deprecated functions
        /wd4244 # Conversion from 'type1' to 'type2'
        /wd4267 # Conversion from 'size_t' to 'type'
    )

    # Enable string pooling
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GF")

    # Enable function-level linking
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Gy")

    # Enable runtime library awareness
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")

    # AVX2 optimizations
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_compile_options(/arch:AVX2)
    endif()

# ============================================================================
# GCC / Clang Flags
# ============================================================================

else()
    # Common flags for both GCC and Clang
    target_compile_options(morphy_interface_options
        INTERFACE
        ${MORPHY_COMMON_WARNINGS}
    )

    # Release optimizations
    set(MORPHY_RELEASE_FLAGS
        -O3
        -march=native
        -ffast-math
        -fno-finite-math-only
        -fno-associative-math
        -funsafe-math-optimizations
        -fno-omit-frame-pointer
        -funroll-loops
        -fmerge-all-constants
        -fvisibility=hidden
        -fvisibility-inlines-hidden
        -flto=thin
        -DNDEBUG
    )

    # Debug flags
    set(MORPHY_DEBUG_FLAGS
        -O0
        -g3
        -fno-omit-frame-pointer
        -fno-inline
        -fno-elide-constructors
    )

    # Clang-specific flags
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(morphy_interface_options
            INTERFACE
            -Wno-unused-private-field
            -Wno-missing-braces
        )

        # clang-tidy integration
        set(CMAKE_CXX_CLANG_TIDY "clang-tidy;-warnings-as-errors=*;")
    endif()

    # GCC-specific flags
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        target_compile_options(morphy_interface_options
            INTERFACE
            -Wno-unused-parameter
            -Wno-maybe-uninitialized
        )
    endif()

    # Sanitizers for debug builds
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        find_package(Sanitizers)
        if(Sanitizers_FOUND)
            add_compile_options(-fsanitize=address -fsanitize=undefined)
            add_link_options(-fsanitize=address -fsanitize=undefined)
        endif()
    endif()

    # LTO (Link Time Optimization) for release
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported)
        if(ipo_supported)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endif()
endif()

# ============================================================================
# Apply Flags
# ============================================================================

# Apply release flags
foreach(flag ${MORPHY_RELEASE_FLAGS})
    add_compile_options($<$<CONFIG:Release>:${flag}>)
endforeach()

# Apply debug flags
foreach(flag ${MORPHY_DEBUG_FLAGS})
    add_compile_options($<$<CONFIG:Debug>:${flag}>)
endforeach()

# ============================================================================
# Platform-Specific Flags
# ============================================================================

if(WIN32)
    add_definitions(-DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
    add_definitions(-DMACOS -D__MACOSX__)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    # Universal binary support
    if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64" OR CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
        # Enable universal binary optimizations
    endif()
elseif(UNIX)
    add_definitions(-DLINUX -D__LINUX__)
endif()

# ============================================================================
# Feature Test Macros
# ============================================================================

# Ensure we have the latest C++ standard features
add_compile_options(
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-exceptions>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang>:-fno-rtti>
)

# Enable constexpr debugging
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(_GLIBCXX_ASSERTIONS=1)
endif()

# ============================================================================
# JUCE-Specific Adjustments
# ============================================================================

# Disable some warnings for JUCE integration
if(MSVC)
    add_compile_options(/wd4100) # Unreferenced formal parameter
    add_compile_options(/wd4127) # Conditional expression is constant
    add_compile_options(/wd4505) # Unreferenced local function
    add_compile_options(/wd4514) # Unreferenced inline function
    add_compile_options(/wd4710) # Function not inlined
    add_compile_options(/wd4711) # Function selected for inline expansion
    add_compile_options(/wd4820) # Bytes padding added after data member
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-deprecated-declarations)
    add_compile_options(-Wno-unused-private-field)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    add_compile_options(-Wno-deprecated-declarations)
    add_compile_options(-Wno-unused-parameter)
endif()
