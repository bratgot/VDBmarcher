# VDBRender — OpenVDB Ray Marcher for Nuke

A Nuke plugin that ray-marches OpenVDB density grids directly in the compositor. Reads `.vdb` files natively — no conversion needed.

## Features

- **Direct .vdb loading** — reads OpenVDB float grids (density, temperature, etc.)
- **VDB sequences** — auto-detects frame numbers, or use `####`/`%04d` patterns
- **Single-scatter lighting** — directional light with shadow rays
- **Axis transform input** — connect an Axis node to move/rotate/scale the volume in 3D
- **3D viewport preview** — bounding box wireframe + density point cloud
- **Proxy rendering** — respects Nuke's proxy/downres settings
- **Format control** — choose output resolution independently
- **Frame offset** — shift sequence timing

## Requirements

- **Windows 11**
- **Visual Studio 2022** with **C++ Clang-cl** compiler
- **CMake 3.20+**
- **Nuke 17** (tested with 17.0v1)
- **vcpkg** with OpenVDB 12

## Build

### 1. Install vcpkg and OpenVDB

```bat
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
bootstrap-vcpkg.bat
vcpkg install openvdb:x64-windows
```

### 2. Install Clang-cl

OpenVDB 12 requires C++20 features that MSVC doesn't fully support. Install Clang-cl:

**Visual Studio Installer → Modify → Individual Components → search "Clang"**
- Tick **C++ Clang Compiler for Windows**
- Tick **C++ Clang-cl for v143 build tools**

### 3. Build

Open an **x64 Native Tools Command Prompt for VS 2022**:

```bat
cd C:\dev\VDBmarcher
mkdir build && cd build

cmake .. -G "NMake Makefiles" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl ^
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DNUKE_ROOT="C:/Program Files/Nuke17.0v1"

nmake
```

### 4. Install

Run `install_vdbrender.bat` or manually:

```bat
mkdir %USERPROFILE%\.nuke\plugins

copy /Y build\VDBRender.dll          %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\openvdb.dll   %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\tbb12.dll     %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\zlib1.dll     %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\blosc.dll     %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\Imath-3_2.dll %USERPROFILE%\.nuke\plugins\
```

Copy `menu.py` into `%USERPROFILE%\.nuke\`.

## Usage

1. **Nodes → VDB → VDBRender**
2. Connect a **Camera** to input 0
3. Optionally connect an **Axis** to input 1 (moves the volume)
4. Set **Output Format** (e.g. HD_1080)
5. Point **VDB File** at your `.vdb` file
6. Use the 3D viewport to see the bounding box and aim the camera

### Inputs

| Input | Label | Type | Required |
|-------|-------|------|----------|
| 0 | cam | Camera | Yes |
| 1 | axis | Axis/Transform | No — moves the volume in world space |

### Sequences

Pick any file from a numbered sequence:
```
dust_impact_0099.vdb
```
The frame number is auto-detected. Also supports `####` and `%04d` patterns.

### Knobs

| Knob | Description |
|------|-------------|
| **Output Format** | Resolution for rendered output. Respects proxy. |
| **VDB File** | Path to .vdb file or sequence |
| **Grid Name** | Density grid name (default: `density`) |
| **Frame Offset** | Shift sequence timing |
| **Step Size** | Ray march step. Start at 0.5, reduce for final. |
| **Extinction** | Light absorption. Higher = denser. |
| **Scattering** | Light scatter. Higher = brighter. |
| **Light Direction** | Normalised internally. |
| **Light Color** | Values >1 = intensity boost. |
| **Show Bounding Box** | Green wireframe in 3D viewport |
| **Show Point Cloud** | Density preview in 3D viewport |
| **Point Density** | Low/Med/High (~16k/64k/250k points) |
| **Point Size** | GL point size for preview |

### Tips

- If you see black, **pull the camera back** and try Extinction=5, Scattering=3
- Use the **3D viewport** point cloud to see the volume shape
- Use **Step Size 0.5–2.0** for fast preview, **0.05–0.1** for final
- Connect an **Axis** to input 1 to reposition the volume without re-exporting
- **Light Color** values above 1.0 act as intensity multipliers
- Lowering proxy in the viewer speeds up rendering proportionally

## Deep Output

Deep (per-sample depth) output is planned for v2. It requires a different base class (`DeepOp`) which is a significant rewrite.

## Technical Notes

- Built with **clang-cl** — needed because OpenVDB 12 uses C++20 features unsupported by MSVC 19.44
- OpenVDB headers included **before** DDImage to avoid `#define foreach` collision
- Camera/Axis data cached in `_validate()` since they're not thread-safe for `engine()`
- Axis transform: rays are transformed into volume-local space via the inverse matrix
- Point cloud uses additive GL blending with density-mapped color ramp

## License

MIT
