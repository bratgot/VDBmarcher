// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: Apache-2.0
// Auto-generated version.h for OpenVDB 12.0.0 (header-only use with Nuke 17)

#ifndef OPENVDB_VERSION_HAS_BEEN_INCLUDED
#define OPENVDB_VERSION_HAS_BEEN_INCLUDED

#include "openvdb/Platform.h"

#include <cstddef>
#include <cstdint>

#define OPENVDB_LIBRARY_MAJOR_VERSION_NUMBER 12
#define OPENVDB_LIBRARY_MINOR_VERSION_NUMBER 0
#define OPENVDB_LIBRARY_PATCH_VERSION_NUMBER 0

#ifndef OPENVDB_ABI_VERSION_NUMBER
#define OPENVDB_ABI_VERSION_NUMBER 12
#endif

#define OPENVDB_LIBRARY_VERSION_STRING "12.0.0"
#define OPENVDB_LIBRARY_ABI_VERSION_STRING "12.0.0abi12"
#define OPENVDB_LIBRARY_VERSION_NUMBER 0x0c000000

#define OPENVDB_PACKAGE_URL ""
#define OPENVDB_PACKAGE_REVISION ""

#if OPENVDB_ABI_VERSION_NUMBER == OPENVDB_LIBRARY_MAJOR_VERSION_NUMBER
    #define OPENVDB_VERSION_NAME v12_0
#else
    #define OPENVDB_VERSION_NAME v12_0abi ## OPENVDB_ABI_VERSION_NUMBER
#endif

#define OPENVDB_USE_IMATH_HALF
#define OPENVDB_IMATH_VERSION 3
#define OPENVDB_USE_BLOSC
#define OPENVDB_USE_ZLIB
#define OPENVDB_USE_DELAYED_LOADING
/* #undef OPENVDB_ENABLE_ASSERTS */
/* Disabled for header-only consumer builds — we link against Nuke's pre-built libopenvdb */
/* #undef OPENVDB_USE_EXPLICIT_INSTANTIATION */

#define OPENVDB_INSTANTIATE extern template OPENVDB_TEMPLATE_IMPORT
#define OPENVDB_INSTANTIATE_CLASS extern template class OPENVDB_TEMPLATE_IMPORT
#define OPENVDB_INSTANTIATE_STRUCT extern template struct OPENVDB_TEMPLATE_IMPORT

#define OPENVDB_REAL_TREE_INSTANTIATE(Function) \
    Function(FloatGrid) \
    Function(DoubleGrid)

#define OPENVDB_NUMERIC_TREE_INSTANTIATE(Function) \
    Function(FloatGrid) \
    Function(DoubleGrid) \
    Function(Int32Grid) \
    Function(Int64Grid)

#define OPENVDB_VEC3_TREE_INSTANTIATE(Function) \
    Function(Vec3SGrid) \
    Function(Vec3DGrid) \
    Function(Vec3IGrid)

#define OPENVDB_VOLUME_TREE_INSTANTIATE(Function) \
    Function(FloatGrid) \
    Function(DoubleGrid) \
    Function(Int32Grid) \
    Function(Int64Grid) \
    Function(BoolGrid) \
    Function(MaskGrid)

#define OPENVDB_ALL_TREE_INSTANTIATE(Function) \
    Function(FloatGrid) \
    Function(DoubleGrid) \
    Function(Int32Grid) \
    Function(Int64Grid) \
    Function(BoolGrid) \
    Function(MaskGrid) \
    Function(Vec3SGrid) \
    Function(Vec3DGrid) \
    Function(Vec3IGrid) \
    Function(StringGrid) \
    Function(PointDataGrid)

#ifdef OPENVDB_REQUIRE_VERSION_NAME
#define OPENVDB_USE_VERSION_NAMESPACE
#else
#define OPENVDB_USE_VERSION_NAMESPACE \
    namespace OPENVDB_VERSION_NAME {} \
    using namespace OPENVDB_VERSION_NAME;
#endif

namespace openvdb {
OPENVDB_USE_VERSION_NAMESPACE
namespace OPENVDB_VERSION_NAME {

const int32_t OPENVDB_MAGIC = 0x56444220;

const uint32_t
    OPENVDB_LIBRARY_MAJOR_VERSION = OPENVDB_LIBRARY_MAJOR_VERSION_NUMBER,
    OPENVDB_LIBRARY_MINOR_VERSION = OPENVDB_LIBRARY_MINOR_VERSION_NUMBER,
    OPENVDB_LIBRARY_PATCH_VERSION = OPENVDB_LIBRARY_PATCH_VERSION_NUMBER;
const uint32_t OPENVDB_LIBRARY_VERSION = OPENVDB_LIBRARY_VERSION_NUMBER;
const uint32_t OPENVDB_ABI_VERSION = OPENVDB_ABI_VERSION_NUMBER;

const uint32_t OPENVDB_FILE_VERSION = 224;

enum {
    OPENVDB_FILE_VERSION_ROOTNODE_MAP = 213,
    OPENVDB_FILE_VERSION_INTERNALNODE_COMPRESSION = 214,
    OPENVDB_FILE_VERSION_SIMPLIFIED_GRID_TYPENAME = 215,
    OPENVDB_FILE_VERSION_GRID_INSTANCING = 216,
    OPENVDB_FILE_VERSION_BOOL_LEAF_OPTIMIZATION = 217,
    OPENVDB_FILE_VERSION_BOOST_UUID = 218,
    OPENVDB_FILE_VERSION_NO_GRIDMAP = 219,
    OPENVDB_FILE_VERSION_NEW_TRANSFORM = 219,
    OPENVDB_FILE_VERSION_SELECTIVE_COMPRESSION = 220,
    OPENVDB_FILE_VERSION_FLOAT_FRUSTUM_BBOX = 221,
    OPENVDB_FILE_VERSION_NODE_MASK_COMPRESSION = 222,
    OPENVDB_FILE_VERSION_BLOSC_COMPRESSION = 223,
    OPENVDB_FILE_VERSION_POINT_INDEX_GRID = 223,
    OPENVDB_FILE_VERSION_MULTIPASS_IO = 224
};

inline constexpr const char* getLibraryVersionString() { return OPENVDB_LIBRARY_VERSION_STRING; }
inline constexpr const char* getLibraryAbiVersionString() {
    return OPENVDB_LIBRARY_ABI_VERSION_STRING;
}
inline constexpr const char* getPackageUrl() { return OPENVDB_PACKAGE_URL; }
inline constexpr const char* getPackageRevision() { return OPENVDB_PACKAGE_REVISION; }

struct VersionId {
    uint32_t first, second;
    VersionId(): first(0), second(0) {}
    VersionId(uint32_t major, uint32_t minor): first(major), second(minor) {}
};

} // namespace OPENVDB_VERSION_NAME
} // namespace openvdb

#endif // OPENVDB_VERSION_HAS_BEEN_INCLUDED
