# VDBRender — OpenVDB Ray Marcher for Nuke 17

Reads `.vdb` files directly in Nuke. No NanoVDB conversion step needed.

## Prerequisites

- **Windows 11**
- **Visual Studio 2022** (or Build Tools) with C++17
- **CMake 3.20+**
- **Nuke 17.0v1** (adjust `NUKE_ROOT` in CMakeLists.txt if different)
- **vcpkg** (for OpenVDB and its dependencies)

## 1. Install OpenVDB via vcpkg

```bat
cd C:\vcpkg
vcpkg install openvdb:x64-windows
```

This pulls in TBB, zlib, blosc, Half/Imath — everything OpenVDB needs.

## 2. Configure & Build

```bat
cd C:\dev\VDBRender
mkdir build && cd build

cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DNUKE_ROOT="C:/Program Files/Nuke17.0v1"

cmake --build . --config Release
```

## 3. Install

Either run:

```bat
install_vdbrender.bat
```

Or manually copy `build/Release/VDBRender.dll` plus the runtime DLLs
(`openvdb.dll`, `tbb12.dll`, `zlib1.dll`, etc.) from
`C:\vcpkg\installed\x64-windows\bin` into `%USERPROFILE%\.nuke\plugins\`.

## 4. Nuke Setup

Copy `menu.py` into `%USERPROFILE%\.nuke\` (or merge with your existing one).

Launch Nuke → **Nodes menu → VDB → VDBRender**.

## Usage

1. Create a **VDBRender** node
2. Connect a **Camera** to input 0
3. Point the **VDB File** knob at a `.vdb` file
4. Set **Grid Name** (defaults to `density`)
5. Adjust step size, extinction, scattering, and light as needed

## What Changed from the NanoVDB Version

| Area | Before (NanoVDB) | After (OpenVDB) |
|------|-------------------|------------------|
| File format | `.nvdb` | `.vdb` |
| Headers | `nanovdb/NanoVDB.h`, header-only | `openvdb/openvdb.h`, linked lib |
| Grid I/O | `nanovdb::io::readGrid` | `openvdb::io::File` |
| Accessor | `nanovdb::FloatGrid::getAccessor()` | `openvdb::FloatGrid::getConstAccessor()` |
| Transform | `grid->worldToIndexF()` | `grid->transform().worldToIndex()` |
| BBox | `grid->worldBBox()` | `evalActiveVoxelBoundingBox()` → manual world conversion |
| Init | none | `openvdb::initialize()` once |
| Dependencies | zlib only | openvdb, TBB, zlib, blosc, Half/Imath |

## Troubleshooting

**"openvdb.dll not found"** — Make sure the vcpkg runtime DLLs are next to
`VDBRender.dll` in the plugins folder, or on your system PATH.

**"tbb12.dll not found"** — Same as above. TBB is a transitive dependency of OpenVDB.

**Grid not rendering** — Check the Nuke error console. Verify the grid name
matches what's in the `.vdb` file (use `vdb_print` from OpenVDB tools to inspect).
