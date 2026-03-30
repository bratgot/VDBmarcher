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

# ── [CUDA] Optional NanoVDB GPU primary ray march ────────────────────────────
# Requires CUDA Toolkit 12+ and an NVIDIA GPU (SM 7.5+ recommended).
# SM architectures: 75=Turing RTX20xx  86=Ampere RTX30xx  89=Ada RTX40xx
option(VDBRENDER_CUDA "Enable CUDA GPU primary ray march (NanoVDB)" OFF)
if(VDBRENDER_CUDA)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_sources(VDBRender PRIVATE VDBRenderKernel.cu)
    target_compile_definitions(VDBRender PRIVATE VDBRENDER_HAS_CUDA=1)
    target_link_libraries(VDBRender PRIVATE CUDA::cudart)
    set_property(TARGET VDBRender PROPERTY CUDA_ARCHITECTURES 75 86 89)
    target_include_directories(VDBRender PRIVATE
        ${CUDAToolkit_INCLUDE_DIRS}
        "${VCPKG_PREFIX}/include"
    )
    set_source_files_properties(VDBRenderKernel.cu
        PROPERTIES COMPILE_OPTIONS "--std=c++17")
    message(STATUS "[CUDA] GPU ray march enabled — SM 75/86/89")
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
install(FILES "${CMAKE_SOURCE_DIR}/menu.py"
    DESTINATION "$ENV{USERPROFILE}/.nuke/plugins/VDBRender/nuke17"
)
install(FILES "${CMAKE_SOURCE_DIR}/init.py"
    DESTINATION "$ENV{USERPROFILE}/.nuke/plugins/VDBRender/nuke17"
)

# Runtime DLLs go in lib/ subfolder — Nuke scans the plugin dir for .dll files
# and will error on any non-plugin DLL. init.py adds lib/ to PATH.
set(VCPKG_BIN_INSTALL "$ENV{VCPKG_ROOT}/installed/x64-windows/bin")
if(NOT EXISTS "${VCPKG_BIN_INSTALL}")
    set(VCPKG_BIN_INSTALL "C:/vcpkg/installed/x64-windows/bin")
endif()

set(INSTALL_RUNTIME_DLLS
    "${VCPKG_BIN_INSTALL}/openvdb.dll"
    "${VCPKG_BIN_INSTALL}/Imath-3_2.dll"
    "${VCPKG_BIN_INSTALL}/tbb12.dll"
    "${VCPKG_BIN_INSTALL}/blosc.dll"
    "${VCPKG_BIN_INSTALL}/zlib1.dll"
    "${VCPKG_BIN_INSTALL}/zstd.dll"
    "${VCPKG_BIN_INSTALL}/lz4.dll"
)

install(FILES ${INSTALL_RUNTIME_DLLS}
    DESTINATION "$ENV{USERPROFILE}/.nuke/plugins/VDBRender/nuke17/lib"
    OPTIONAL
)

# ── Distribution folder ────────────────────────────────────────────────────────
# Builds a self-contained dist/ folder ready to zip and share.
# Contents:
#   dist/VDBRender_v3/
#     VDBRender.dll          — plugin
#     init.py                — DLL path setup (runs before plugin load)
#     menu.py                — Nuke menu entries
#     lib/                   — runtime dependencies (not scanned by Nuke)
#       openvdb.dll
#       Imath-3_2.dll
#       tbb12.dll
#       blosc.dll
#       zlib1.dll
#       zstd.dll
#       lz4.dll
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
     %USERPROFILE%\\.nuke\\plugins\\VDBRender\\nuke17\\VDBRender.dll
     %USERPROFILE%\\.nuke\\plugins\\VDBRender\\nuke17\\init.py
     %USERPROFILE%\\.nuke\\plugins\\VDBRender\\nuke17\\menu.py
     %USERPROFILE%\\.nuke\\plugins\\VDBRender\\nuke17\\lib\\openvdb.dll  (etc.)

3. Add this line to %USERPROFILE%\\.nuke\\init.py (create it if missing):
     nuke.pluginAddPath('./plugins/VDBRender/nuke17')

   This runs init.py which sets up DLL paths before Nuke loads plugins.
   The VDB menu is created automatically by menu.py.

4. Restart Nuke. VDBRender appears in the VDB menu.

REQUIREMENTS
============
- Nuke 17.0v1 or later
- Windows x64

The lib/ subfolder contains runtime DLLs that must stay alongside
the plugin. Do not move them out of lib/ or into the parent folder
— Nuke will try to load any .dll in the plugin directory as a plugin.

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

    # Copy menu script and init.py
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/menu.py"
        "${DIST_DIR}/menu.py"

    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/init.py"
        "${DIST_DIR}/init.py"

    # Copy third party licenses
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.txt"
        "${DIST_DIR}/THIRD_PARTY_LICENSES.txt"

    # Copy runtime DLLs into lib/ subfolder (Nuke scans plugin dir for .dll)
    COMMAND ${CMAKE_COMMAND} -E make_directory "${DIST_DIR}/lib"
    COMMAND ${CMAKE_COMMAND} -E echo "Copying runtime DLLs to lib/ from ${VCPKG_BIN}..."
)

# Copy each runtime DLL into lib/ subfolder
foreach(DLL_PATH ${DIST_RUNTIME_DLLS})
    get_filename_component(DLL_NAME "${DLL_PATH}" NAME)
    add_custom_command(TARGET dist POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${DLL_PATH}"
            "${DIST_DIR}/lib/${DLL_NAME}"
        COMMENT "  lib/${DLL_NAME}"
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

# ── make_cd_test — standalone VDB generator for Cd scatter albedo testing ──────
add_executable(make_cd_test make_cd_test.cpp)
target_link_libraries(make_cd_test PRIVATE OpenVDB::openvdb)
set_property(TARGET make_cd_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

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
if(VDBRENDER_CUDA)
    message(STATUS "    CUDA:        ENABLED (SM 75/86/89)")
else()
    message(STATUS "    CUDA:        disabled (use -DVDBRENDER_CUDA=ON to enable)")
endif()
message(STATUS "")
