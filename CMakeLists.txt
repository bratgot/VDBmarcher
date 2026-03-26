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
    ${NDK_LIB}
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

# ── Install ────────────────────────────────────────────────────────────────────
install(TARGETS VDBRender
    DESTINATION "${NUKE_ROOT}/plugins"
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
