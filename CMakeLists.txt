cmake_minimum_required(VERSION 3.20)
project(VDBRender CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Nuke install path ──────────────────────────────────────────────────────────
set(NUKE_ROOT "" CACHE PATH "Nuke install directory (e.g. C:/Program Files/Nuke17.0v1)")

set(NDK_INCLUDE_DIR "${NUKE_ROOT}/include")
set(NDK_LIB         "${NUKE_ROOT}/DDImage.lib")

# ── OpenVDB ────────────────────────────────────────────────────────────────────
# Install via vcpkg:  vcpkg install openvdb:x64-windows
# Then configure with: -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
# Nuke 16+ only — Nuke 14-15 have incompatible TBB.
find_package(OpenVDB REQUIRED)

# ── Plugin target ──────────────────────────────────────────────────────────────
add_library(VDBRender SHARED
    VDBRenderIop.cpp
)

target_include_directories(VDBRender PRIVATE
    ${NDK_INCLUDE_DIR}
)

target_compile_definitions(VDBRender PRIVATE
    _USE_MATH_DEFINES
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    FN_SHARED_IMPORT=__declspec\(dllimport\)
    FN_SHARED_EXPORT=__declspec\(dllexport\)
)

target_link_libraries(VDBRender PRIVATE
    ${NDK_LIB}
    OpenVDB::openvdb
    opengl32
)

# Nuke expects no lib prefix, .dll suffix
set_target_properties(VDBRender PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
)

# Match Nuke's MSVC runtime (/MD Release)
set_property(TARGET VDBRender PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

# ── Install ────────────────────────────────────────────────────────────────────
install(TARGETS VDBRender
    DESTINATION "${NUKE_ROOT}/plugins"
)
