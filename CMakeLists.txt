cmake_minimum_required(VERSION 3.20)
project(VDBRender CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Nuke install path ──────────────────────────────────────────────────────────
set(NUKE_ROOT "" CACHE PATH "Nuke install directory (e.g. C:/Program Files/Nuke17.0v1)")

set(NDK_INCLUDE_DIR "${NUKE_ROOT}/include")
set(NDK_LIB         "${NUKE_ROOT}/DDImage.lib")

# ── OpenVDB ────────────────────────────────────────────────────────────────────
find_package(OpenVDB REQUIRED)

# ── [NEURAL] Optional LibTorch for NeuralVDB support ──────────────────────────
# Download LibTorch from https://pytorch.org (C++ / cxx11 ABI)
# Then configure with: -DLIBTORCH_PATH=C:/libtorch
option(VDBRENDER_NEURAL "Enable NeuralVDB neural compressed volume support" OFF)
set(LIBTORCH_PATH "" CACHE PATH "Path to LibTorch C++ distribution")

if(VDBRENDER_NEURAL)
    if(LIBTORCH_PATH AND EXISTS "${LIBTORCH_PATH}/share/cmake/Torch/TorchConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${LIBTORCH_PATH}")
        find_package(Torch REQUIRED)
        message(STATUS "[NeuralVDB] LibTorch found: ${TORCH_LIBRARIES}")
    else()
        message(FATAL_ERROR
            "[NeuralVDB] VDBRENDER_NEURAL=ON but LibTorch not found.\n"
            "Set LIBTORCH_PATH to your LibTorch installation, e.g.:\n"
            "  -DLIBTORCH_PATH=C:/libtorch\n"
            "Download from: https://pytorch.org/get-started/locally/")
    endif()
endif()

# ── Plugin target ──────────────────────────────────────────────────────────────
add_library(VDBRender SHARED
    VDBRenderIop.cpp
)

target_include_directories(VDBRender PRIVATE
    ${NDK_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}    # for NeuralDecoder.h
)

target_compile_definitions(VDBRender PRIVATE
    _USE_MATH_DEFINES
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    FN_SHARED_IMPORT=__declspec\(dllimport\)
    FN_SHARED_EXPORT=__declspec\(dllexport\)
)

# [NEURAL] Add neural define when enabled
if(VDBRENDER_NEURAL)
    target_compile_definitions(VDBRender PRIVATE VDBRENDER_HAS_NEURAL=1)
    target_include_directories(VDBRender PRIVATE ${TORCH_INCLUDE_DIRS})
endif()

# AVX2 SIMD + fast math for volume ray marching
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(VDBRender PRIVATE -mavx2 -mfma -ffast-math)
elseif(MSVC)
    target_compile_options(VDBRender PRIVATE /arch:AVX2 /fp:fast)
endif()

target_link_libraries(VDBRender PRIVATE
    "${NDK_LIB}"
    "${NUKE_ROOT}/ndk.lib"
    OpenVDB::openvdb
    opengl32
)

# [NEURAL] Link LibTorch when enabled
if(VDBRENDER_NEURAL)
    target_link_libraries(VDBRender PRIVATE ${TORCH_LIBRARIES})
endif()

set_target_properties(VDBRender PROPERTIES
    PREFIX ""
    SUFFIX ".dll"
)

set_property(TARGET VDBRender PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

# ── Install (to Nuke plugins dir) ─────────────────────────────────────────────
install(TARGETS VDBRender
    DESTINATION "$ENV{USERPROFILE}/.nuke/plugins/VDBRender/nuke17"
)
install(FILES "${CMAKE_SOURCE_DIR}/VDBRender_menu.py"
    DESTINATION "$ENV{USERPROFILE}/.nuke/plugins/VDBRender"
)

# ── Distribution folder ────────────────────────────────────────────────────────
# Builds a self-contained dist/ folder ready to zip and share.
# Contents:
#   dist/VDBRender_v3/
#     VDBRender.dll          — plugin
#     VDBRender_menu.py      — Nuke menu loader
#     openvdb.dll            — runtime dependencies
#     Imath-3_2.dll
#     tbb12.dll
#     blosc.dll
#     zlib1.dll
#     zstd.dll
#     lz4.dll
#     INSTALL.txt            — one-page setup instructions

set(DIST_DIR "${CMAKE_SOURCE_DIR}/dist/VDBRender_v3")

set(VCPKG_BIN "$ENV{VCPKG_ROOT}/installed/x64-windows/bin")
if(NOT EXISTS "${VCPKG_BIN}")
    set(VCPKG_BIN "C:/vcpkg/installed/x64-windows/bin")
endif()

set(DIST_RUNTIME_DLLS
    "${VCPKG_BIN}/openvdb.dll"
    "${VCPKG_BIN}/Imath-3_2.dll"
    "${VCPKG_BIN}/tbb12.dll"
    "${VCPKG_BIN}/blosc.dll"
    "${VCPKG_BIN}/zlib1.dll"
    "${VCPKG_BIN}/zstd.dll"
    "${VCPKG_BIN}/lz4.dll"
)

# INSTALL.txt content
set(INSTALL_TXT
"VDBRender v3 — OpenVDB Volume Ray Marcher for Nuke 17
Created by Marten Blumen
https://github.com/bratgot/VDBmarcher

INSTALLATION
============
1. Rename this folder from VDBRender_v3 to VDBRender

2. Copy the VDBRender/ folder to:
     %USERPROFILE%\\.nuke\\plugins\\

   So the result is:
     %USERPROFILE%\\.nuke\\plugins\\VDBRender\\VDBRender.dll
     %USERPROFILE%\\.nuke\\plugins\\VDBRender\\VDBRender_menu.py
     %USERPROFILE%\\.nuke\\plugins\\VDBRender\\openvdb.dll  (etc.)

3. Add this line to %USERPROFILE%\\.nuke\\menu.py:
     import VDBRender.VDBRender_menu

4. Restart Nuke. VDBRender appears in the VDB menu.

   NOTE: The folder must be named VDBRender (not VDBRender_v3)
   for the Python import to work correctly.

REQUIREMENTS
============
- Nuke 17.0v1 or later
- Windows x64

All DLLs in this folder must remain alongside VDBRender.dll.
Do not move VDBRender.dll to a different directory.

USAGE
=====
1. Create a VDBRender node from the VDB menu
2. Connect Camera to input 1, Scene (with lights) to input 2
3. Set the VDB File path and click Discover Grids
4. Choose a Scene Preset and render

For clouds: enable Shadow Cache (Quality tab) and set
Env Mode to 'SH + Virtual Lights' (Lighting tab) for
best performance.

WHAT'S NEW IN V3
================
- Dual-lobe HG and approximate Mie phase functions
- Powder effect, analytical multiple scattering, ray jitter
- Procedural fBm detail noise
- HDDA shadow rays + optional transmittance cache (5-10x faster)
- SH environment lighting (10-20x faster for clouds)
- Virtual directional lights from HDRI peaks with shadowing
- CIE 1931 blackbody (accurate to 40000K)
- Distribution build target (this folder)

Full changelog: https://github.com/bratgot/VDBmarcher
")

add_custom_target(dist
    DEPENDS VDBRender
    COMMENT "Building distribution package in dist/VDBRender_v3/"

    # Create dist dir
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}"

    # Copy DLL
    COMMAND ${CMAKE_COMMAND} -E copy
        "$<TARGET_FILE:VDBRender>"
        "${DIST_DIR}/VDBRender.dll"

    # Copy menu script
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/VDBRender_menu.py"
        "${DIST_DIR}/VDBRender_menu.py"

    # Copy third party licenses
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.txt"
        "${DIST_DIR}/THIRD_PARTY_LICENSES.txt"

    # Copy runtime DLLs (warn but don't fail if a DLL is missing)
    COMMAND ${CMAKE_COMMAND} -E echo "Copying runtime DLLs from ${VCPKG_BIN}..."
)

# Copy each runtime DLL individually
foreach(DLL_PATH ${DIST_RUNTIME_DLLS})
    get_filename_component(DLL_NAME "${DLL_PATH}" NAME)
    add_custom_command(TARGET dist POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL_PATH}"
            "${DIST_DIR}/${DLL_NAME}"
        COMMENT "  ${DLL_NAME}"
    )
endforeach()

# Write INSTALL.txt
file(WRITE "${CMAKE_SOURCE_DIR}/dist/INSTALL.txt.in" "${INSTALL_TXT}")
add_custom_command(TARGET dist POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/dist/INSTALL.txt.in"
        "${DIST_DIR}/INSTALL.txt"
    COMMENT "  INSTALL.txt"
)

# Summary
add_custom_command(TARGET dist POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Distribution ready: ${DIST_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "Zip the VDBRender/ folder and share."
)

# ── [NEURAL] nvdb_encode command-line encoder ─────────────────────────────────
# Standalone .exe that reads .vdb (vcpkg OpenVDB) and writes .nvdb (LibTorch)
# No Nuke NDK dependency — just OpenVDB + LibTorch
if(VDBRENDER_NEURAL)
    add_executable(nvdb_encode nvdb_encode.cpp)

    target_compile_definitions(nvdb_encode PRIVATE
        _USE_MATH_DEFINES
        NOMINMAX
    )

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(nvdb_encode PRIVATE -mavx2 -mfma -ffast-math)
    elseif(MSVC)
        target_compile_options(nvdb_encode PRIVATE /arch:AVX2 /fp:fast)
    endif()

    target_link_libraries(nvdb_encode PRIVATE
        OpenVDB::openvdb
        ${TORCH_LIBRARIES}
    )

    set_property(TARGET nvdb_encode PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    )

    message(STATUS "    Encoder:     nvdb_encode.exe")
endif()

# ── Status ─────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "  VDBRender configuration:")
message(STATUS "    Nuke:        ${NUKE_ROOT}")
message(STATUS "    OpenVDB:     found")
if(VDBRENDER_NEURAL)
    message(STATUS "    NeuralVDB:   ENABLED (LibTorch: ${LIBTORCH_PATH})")
else()
    message(STATUS "    NeuralVDB:   disabled (use -DVDBRENDER_NEURAL=ON to enable)")
endif()
message(STATUS "")
