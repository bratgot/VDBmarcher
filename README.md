# VDBRender — OpenVDB Volume Ray Marcher for Nuke

A single-scatter volumetric ray marcher that reads OpenVDB density grids directly inside the Nuke compositor. No external renderer or shader setup required.

**Created by Marten Blumen**

## Features

- **Native .vdb loading** with auto-detected frame sequences
- **6 render modes** — Lit, Greyscale, Heat, Cool, Blackbody, Custom Gradient
- **Physically-based blackbody** — Planckian locus temperature-to-color mapping
- **Temperature grid emission** — auto-detects `temperature`, `heat`, `flame`, `flames`, `fire`, `temp` grids
- **Henyey-Greenstein phase function** — anisotropic forward/back scatter
- **Up to 8 Nuke Light nodes** — point light support with per-sample direction, color, and intensity
- **Axis transform input** — move/rotate/scale volume via connected Axis node
- **Interactive 3D viewport** — bounding box wireframe + density point cloud preview
- **Proxy-aware rendering** — respects Nuke's downres settings

## Inputs

| Input | Label | Type | Description |
|-------|-------|------|-------------|
| 0 | cam | Camera | Required — defines the render viewpoint |
| 1 | axis | Axis/TransformGeo | Optional — transforms the volume in world space |
| 2–9 | light1–8 | Light | Optional — point lights with position, color, intensity |

## Render Modes

| Mode | Description |
|------|-------------|
| **Lit** | Single-scatter lighting with shadow rays and phase function |
| **Greyscale** | Density mapped to luminance |
| **Heat** | Black → Red → Yellow → White |
| **Cool** | Black → Blue → Cyan → White |
| **Blackbody** | Planckian locus. Density or temperature grid → Kelvin range |
| **Custom Gradient** | User-defined two-color ramp |

## Phase Function

The **Anisotropy (g)** parameter controls the Henyey-Greenstein phase function:

| Value | Effect |
|-------|--------|
| -1.0 | Strong back-scatter — rim-light/halo effect |
| 0.0 | Isotropic — uniform scatter in all directions |
| +0.3 to +0.6 | Realistic smoke/cloud look |
| +1.0 | Strong forward-scatter — volume glows when backlit |

## Requirements

- **Windows 11**
- **Visual Studio 2022** with **C++ Clang-cl** (Individual Components)
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

Visual Studio Installer → Modify → Individual Components:
- **C++ Clang Compiler for Windows**
- **C++ Clang-cl for v143 build tools**

### 3. Build

Open **x64 Native Tools Command Prompt for VS 2022**:

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
copy /Y build\VDBRender.dll                               %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\openvdb.dll    %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\tbb12.dll      %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\zlib1.dll      %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\blosc.dll      %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\Imath-3_2.dll  %USERPROFILE%\.nuke\plugins\
```

Copy `menu.py` into `%USERPROFILE%\.nuke\`.

## Quick Start

1. **Nodes → VDB → VDBRender**
2. Connect a **Camera** to input 0
3. Set **Output Format** (e.g. HD_1080)
4. Browse to a `.vdb` file
5. Connect a **Light** to input 2
6. Adjust **Extinction**, **Scattering**, **Anisotropy**

For fire/explosions: set **Render Mode** to Blackbody, adjust **Temp Min/Max** (try 500–3000K), and increase **Emission Intensity** if the file has a temperature grid.

## Technical Notes

- Built with **clang-cl** (Clang 19 / MSVC ABI) — required because OpenVDB 12 uses C++20 constexpr features unsupported by MSVC 19.44
- OpenVDB headers must be included **before** DDImage headers (`#define foreach` collision)
- Camera, Axis, and Light data cached in `_validate()` — `CameraOp`/`LightOp` are not thread-safe for `engine()` calls
- All upstream 3D ops are validated early so `append()` can hash their transforms for correct cache invalidation
- Light positions are transformed into volume-local space when an Axis is connected
- Shadow ray step scales to the bounding box diagonal for consistent shadow quality at any volume size

## License

MIT

---

*Created by Marten Blumen · OpenVDB 12.0 · clang-cl 19 · C++20 · Nuke NDK 17*
