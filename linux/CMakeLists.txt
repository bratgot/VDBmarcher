# VDBRender — Linux build
#
# Builds the VDBRender Nuke plugin (.so) against a Nuke installation's own
# bundled OpenVDB and oneTBB libraries, using matching upstream headers.
# See ../LINUX_BUILD.md for the full story and ../build_linux.sh for the
# driver script that sets the cache variables below.
#
# Linux port by Soup Kitchen Films (https://github.com/soupkitchenfilms),
# developed with the assistance of Claude (Anthropic).
# Original plugin by Marten Blumen (https://github.com/bratgot/VDBmarcher), MIT.

cmake_minimum_required(VERSION 3.20)
project(VDBRender CXX)

# NDK docs specify C++17, and the source uses std::clamp (C++17).
# Do NOT raise to C++20: Nuke 17's GeoInfo.h RefCountedPtr hits an
# aggregate-initialisation error under C++20.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)

# ── Configuration ──────────────────────────────────────────────────────────────
set(NUKE_ROOT "" CACHE PATH "Nuke install directory (e.g. /usr/local/Nuke17.0v3)")
set(OPENVDB_HEADER_DIR "" CACHE PATH "OpenVDB source headers matching Nuke's bundled libopenvdb (dir containing openvdb/openvdb/*.h)")
set(ONETBB_HEADER_DIR "" CACHE PATH "oneTBB headers matching Nuke's bundled libtbb (the include/ dir)")

# ── Nuke NDK via Foundry's CMake config ───────────────────────────────────────
list(APPEND CMAKE_PREFIX_PATH "${NUKE_ROOT}/cmake")
find_package(Nuke REQUIRED)

# ── OpenVDB: headers from source checkout, library from Nuke ──────────────────
# Nuke ships its own libopenvdb.so; linking against it (rather than a second
# OpenVDB build) avoids two copies of OpenVDB in one process.
find_library(OPENVDB_LIBRARY
    NAMES openvdb
    PATHS "${NUKE_ROOT}"
    NO_DEFAULT_PATH
)
if(NOT OPENVDB_LIBRARY)
    message(FATAL_ERROR "Could not find libopenvdb.so in ${NUKE_ROOT}")
endif()
message(STATUS "OpenVDB library: ${OPENVDB_LIBRARY}")

# ── TBB: link against Nuke's bundled oneTBB ───────────────────────────────────
# System tbb-devel on EL9 is TBB 2020.3, whose tbb::task API was removed in
# oneTBB — compiling against it links but fails at runtime with
# "undefined symbol: _ZTIN3tbb4taskE". Headers must match Nuke's oneTBB.
find_library(TBB_LIBRARY
    NAMES tbb
    PATHS "${NUKE_ROOT}"
    NO_DEFAULT_PATH
)
if(NOT TBB_LIBRARY)
    message(FATAL_ERROR "Could not find libtbb.so in ${NUKE_ROOT}")
endif()
message(STATUS "TBB library: ${TBB_LIBRARY}")

find_package(OpenGL REQUIRED)

# ── Plugin target ──────────────────────────────────────────────────────────────
add_library(VDBRender MODULE
    "${CMAKE_CURRENT_SOURCE_DIR}/../VDBRenderIop.cpp"
)

target_include_directories(VDBRender PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/.."     # NeuralDecoder.h and friends
    "${ONETBB_HEADER_DIR}"               # before NUKE_ROOT/include and any system TBB
    "${NUKE_ROOT}/include"
    "${OPENVDB_HEADER_DIR}"
)

# NDK requires the CXX11 ABI on Nuke 15+
target_compile_definitions(VDBRender PRIVATE
    _GLIBCXX_USE_CXX11_ABI=1
)

# AVX2 SIMD + fast math, matching the upstream Windows build flags
target_compile_options(VDBRender PRIVATE
    -mavx2 -mfma -ffast-math
)

target_link_libraries(VDBRender PRIVATE
    Nuke::NDK
    ${OPENVDB_LIBRARY}
    ${TBB_LIBRARY}
    OpenGL::GL
)

# Nuke expects no lib prefix and a .so suffix
set_target_properties(VDBRender PROPERTIES
    PREFIX ""
    SUFFIX ".so"
)

install(TARGETS VDBRender DESTINATION "${CMAKE_INSTALL_PREFIX}")
