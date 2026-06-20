# cmake/FetchOnnxRuntime.cmake
#
# Downloads the official prebuilt ONNX Runtime release for the current
# platform/arch and exposes it as the imported target `more_phi::onnxruntime`.
# Invoked only when MORE_PHI_ENABLE_ONNX=ON.
#
# Why prebuilt binaries (not FetchContent source build):
#   - Source builds drag in onnx + protobuf and add ~30+ min to configure.
#   - The prebuilt dynamic lib (onnxruntime.dll / .so / .dylib) is Microsoft's
#     supported distribution for embedding and is what the C++ exporter's
#     opset-18 graphs are validated against.
#
# The download lands in the CMake cache dir (_deps/onnxruntime-prebuilt) so it
# is fetched once and reused across clean reconfigures. The runtime DLL/.so is
# copied next to the built plugin/test exes via a POST_BUILD step so the host
# process can load it.

set(ORT_VERSION "${MORE_PHI_ONNX_VERSION}")
set(ORT_CACHE_DIR "${CMAKE_BINARY_DIR}/_deps/onnxruntime-prebuilt")

# ── Resolve the per-platform asset URL + internal layout ─────────────────────
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ORT_ASSET "onnxruntime-win-x64-${ORT_VERSION}.zip")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ASSET}")
    set(ORT_TOPDIR "onnxruntime-win-x64-${ORT_VERSION}")  # dir inside the zip
    set(ORT_LIB "lib/onnxruntime.lib")
    set(ORT_DLL "lib/onnxruntime.dll")
    set(ORT_INCLUDE "include")
elseif(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(ORT_ASSET "onnxruntime-osx-arm64-${ORT_VERSION}.tgz")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ASSET}")
    set(ORT_TOPDIR "onnxruntime-osx-arm64-${ORT_VERSION}")
    set(ORT_LIB "lib/libonnxruntime.dylib")
    set(ORT_DYLIB "lib/libonnxruntime.dylib")
    set(ORT_INCLUDE "include")
elseif(APPLE)
    set(ORT_ASSET "onnxruntime-osx-x86_64-${ORT_VERSION}.tgz")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ASSET}")
    set(ORT_TOPDIR "onnxruntime-osx-x86_64-${ORT_VERSION}")
    set(ORT_LIB "lib/libonnxruntime.dylib")
    set(ORT_DYLIB "lib/libonnxruntime.dylib")
    set(ORT_INCLUDE "include")
elseif(UNIX AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ORT_ASSET "onnxruntime-linux-x64-${ORT_VERSION}.tgz")
    set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ASSET}")
    set(ORT_TOPDIR "onnxruntime-linux-x64-${ORT_VERSION}")
    set(ORT_LIB "lib/libonnxruntime.so")
    set(ORT_SONAME "lib/libonnxruntime.so.${ORT_VERSION}")
    set(ORT_INCLUDE "include")
else()
    message(FATAL_ERROR
        "MORE_PHI_ENABLE_ONNX: no prebuilt ONNX Runtime asset for this platform "
        "(system='${CMAKE_SYSTEM_NAME}' proc='${CMAKE_SYSTEM_PROCESSOR}'). "
        "Set MORE_PHI_ENABLE_ONNX=OFF or vendor ORT manually.")
endif()

set(ORT_ROOT "${ORT_CACHE_DIR}/${ORT_TOPDIR}")

# ── Download + extract once (idempotent on the extracted marker) ─────────────
set(ORT_ARCHIVE "${ORT_CACHE_DIR}/${ORT_ASSET}")
set(ORT_MARKER "${ORT_ROOT}/.more_phi_extracted")
if(NOT EXISTS "${ORT_MARKER}")
    message(STATUS "ONNX Runtime: downloading ${ORT_ASSET} ...")
    file(MAKE_DIRECTORY "${ORT_CACHE_DIR}")
    file(DOWNLOAD "${ORT_URL}" "${ORT_ARCHIVE}"
         STATUS ORT_DL_STATUS SHOW_PROGRESS TIMEOUT 600)
    list(GET ORT_DL_STATUS 0 ORT_DL_RC)
    if(ORT_DL_RC)
        list(GET ORT_DL_STATUS 1 ORT_DL_ERR)
        message(FATAL_ERROR "ONNX Runtime download failed (${ORT_DL_RC}): ${ORT_DL_ERR}\nURL: ${ORT_URL}")
    endif()
    message(STATUS "ONNX Runtime: extracting ...")
    if(ORT_ASSET MATCHES "\\.zip$")
        file(ARCHIVE_EXTRACT INPUT "${ORT_ARCHIVE}" DESTINATION "${ORT_CACHE_DIR}")
    else()
        file(ARCHIVE_EXTRACT INPUT "${ORT_ARCHIVE}" DESTINATION "${ORT_CACHE_DIR}")
    endif()
    file(WRITE "${ORT_MARKER}" "${ORT_VERSION}")
endif()

if(NOT EXISTS "${ORT_ROOT}/${ORT_LIB}")
    message(FATAL_ERROR
        "ONNX Runtime: expected lib not found at ${ORT_ROOT}/${ORT_LIB}. "
        "The prebuilt archive layout may have changed for v${ORT_VERSION}; "
        "inspect ${ORT_ROOT} and update FetchOnnxRuntime.cmake.")
endif()

# ── Imported target ──────────────────────────────────────────────────────────
add_library(more_phi_onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(more_phi_onnxruntime PROPERTIES
    IMPORTED_LOCATION "${ORT_ROOT}/${ORT_DLL}${ORT_DYLIB}${ORT_SONAME}"
    IMPORTED_IMPLIB   "${ORT_ROOT}/${ORT_LIB}"
)
# On Windows IMPORTED_LOCATION must point at the .dll; on Unix at the .so.
# CMake picks the right property per platform; set both where they exist.
if(WIN32)
    file(TO_NATIVE_PATH "${ORT_ROOT}/${ORT_DLL}" ORT_DLL_NATIVE)
endif()

target_include_directories(more_phi_onnxruntime INTERFACE "${ORT_ROOT}/${ORT_INCLUDE}")

# Alias so consumers link `more_phi::onnxruntime` (the name used by the runner).
add_library(more_phi::onnxruntime ALIAS more_phi_onnxruntime)

# Expose the runtime path so subdirectory scopes (tests/CMakeLists.txt) can copy
# it next to exes (Windows DLL resolution + Linux/macOS rpath convenience).
# CACHE INTERNAL is used instead of PARENT_SCOPE because PARENT_SCOPE only
# propagates one level up the directory stack, and FetchOnnxRuntime.cmake is
# include()-d into the top-level scope while tests/ lives in a child scope added
# via add_subdirectory(). A cache var crosses that boundary and survives
# reconfigures, so the test-exe DLL-copy step actually fires on Windows.
set(MORE_PHI_ONNX_RUNTIME_LIB
    "${ORT_ROOT}/${ORT_DLL}${ORT_DYLIB}${ORT_SONAME}"
    CACHE INTERNAL "Path to the prebuilt ONNX Runtime shared lib (for POST_BUILD copy)")

message(STATUS "ONNX Runtime: ready at ${ORT_ROOT} (v${ORT_VERSION})")
