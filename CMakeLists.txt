cmake_minimum_required(VERSION 3.20)
project(VDBRender CXX)

set(CMAKE_CXX_STANDARD 20 CACHE STRING "C++ standard (17 for Nuke 14-15, 20 for 16+)")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Nuke install path ──────────────────────────────────────────────────────────
set(NUKE_ROOT "" CACHE PATH "Nuke install directory (e.g. C:/Program Files/Nuke17.0v1)")

set(NDK_INCLUDE_DIR "${NUKE_ROOT}/include")
set(NDK_LIB         "${NUKE_ROOT}/DDImage.lib")

# ── OpenVDB ────────────────────────────────────────────────────────────────────
option(OPENVDB_STATIC "Manually link static OpenVDB (for x64-windows-static-md)" OFF)

if(OPENVDB_STATIC)
    # Static build: bypass FindOpenVDB, link everything manually
    set(VCPKG_STATIC_DIR "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
        CACHE PATH "vcpkg static-md install root")
    set(VDB_STATIC_INCLUDE "${VCPKG_STATIC_DIR}/include")
    set(VDB_STATIC_LIB_DIR "${VCPKG_STATIC_DIR}/lib")

    # Core static lib
    find_library(VDB_LIB NAMES libopenvdb PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)

    # All transitive static deps
    find_library(TBB_LIB NAMES tbb12 PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(HALF_LIB NAMES Imath-3_2 PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(BLOSC_LIB NAMES libblosc PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(ZLIB_LIB NAMES zlib PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(ZSTD_LIB NAMES zstd PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(LZ4_LIB NAMES lz4 PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_library(BOOST_IO_LIB NAMES boost_iostreams-vc143-mt-x64-1_90 boost_iostreams PATHS "${VDB_STATIC_LIB_DIR}" NO_DEFAULT_PATH)

    set(OPENVDB_LIBRARIES
        ${VDB_LIB} ${TBB_LIB} ${HALF_LIB} ${BLOSC_LIB}
        ${ZLIB_LIB} ${ZSTD_LIB} ${LZ4_LIB})
    if(BOOST_IO_LIB)
        list(APPEND OPENVDB_LIBRARIES ${BOOST_IO_LIB})
    endif()
    set(OPENVDB_INCLUDE_DIR "${VDB_STATIC_INCLUDE}")

    message(STATUS "OpenVDB static: ${VDB_LIB}")
else()
    # Dynamic build: use normal find_package
    find_package(OpenVDB REQUIRED)
endif()

# ── Plugin target ──────────────────────────────────────────────────────────────
add_library(VDBRender SHARED
    VDBRenderIop.cpp
)

target_include_directories(VDBRender PRIVATE
    ${NDK_INCLUDE_DIR}
    $<$<BOOL:${OPENVDB_STATIC}>:${OPENVDB_INCLUDE_DIR}>
)

target_compile_definitions(VDBRender PRIVATE
    _USE_MATH_DEFINES
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    FN_SHARED_IMPORT=__declspec\(dllimport\)
    FN_SHARED_EXPORT=__declspec\(dllexport\)
    $<$<BOOL:${OPENVDB_STATIC}>:OPENVDB_STATICLIB>
)

if(OPENVDB_STATIC)
    target_link_libraries(VDBRender PRIVATE
        ${NDK_LIB}
        ${OPENVDB_LIBRARIES}
        opengl32
    )
else()
    target_link_libraries(VDBRender PRIVATE
        ${NDK_LIB}
        OpenVDB::openvdb
        opengl32
    )
endif()

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
