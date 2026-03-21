# VDBRender — OpenVDB Volume Ray Marcher for Nuke

A single-scatter volumetric ray marcher that reads OpenVDB density, temperature, and flame grids directly inside the Nuke compositor. No external renderer or shader setup required.

**Created by Marten Blumen**

## Features

- **Native .vdb loading** with auto-detected frame sequences
- **7 render modes** — Lit, Greyscale, Heat, Cool, Blackbody, Custom Gradient, Explosion
- **Explosion mode** — lit smoke (density) + self-luminous fire (temperature/flames)
- **3 grid slots** — Density, Temperature, Flames with per-grid mix sliders
- **Discover Grids** — auto-scans VDB files and populates grid fields
- **9 scene presets** — Thin Smoke, Dense Smoke, Fog, Cloud, Fire, Explosion, Pyroclastic, Dust Storm, Steam
- **Henyey-Greenstein phase function** — anisotropic forward/back scatter with material presets
- **Physically-based blackbody** — Planckian locus temperature-to-color mapping
- **Up to 8 Nuke Light nodes** — point light support with per-sample direction
- **Axis transform input** — move/rotate/scale volume via connected Axis node
- **Interactive 3D viewport** — bounding box wireframe + density point cloud preview
- **Logarithmic quality slider** — artist-friendly 1–10 range
- **Master intensity** — global brightness for all render modes
- **Ambient fill light** — omnidirectional scatter ignoring shadows
- **Shadow density** — separate control for shadow darkness
- **Proxy-aware rendering** — respects Nuke's downres settings

## Inputs

| Input | Label | Type | Description |
|-------|-------|------|-------------|
| 0 | cam | Camera | Required — defines the render viewpoint |
| 1 | axis | Axis | Optional — transforms the volume in world space |
| 2–9 | light1–8 | Light | Optional — point lights with position, color, intensity |

## Scene Presets

| Preset | Mode | Extinction | Scatter | Anisotropy | Quality |
|--------|------|-----------|---------|------------|---------|
| Thin Smoke | Lit | 2 | 1.5 | 0.4 | 2 |
| Dense Smoke | Lit | 15 | 4 | 0.35 | 3 |
| Fog / Mist | Lit | 0.5 | 0.8 | 0.8 | 1 |
| Cumulus Cloud | Lit | 12 | 10 | 0.76 | 5 |
| Fire | Explosion | 5 | 2 | 0.3 | 3 |
| Explosion | Explosion | 20 | 5 | 0.4 | 5 |
| Pyroclastic | Explosion | 30 | 6 | 0.5 | 7 |
| Dust Storm | Lit | 4 | 3 | -0.3 | 2 |
| Steam | Lit | 1.5 | 2 | 0.7 | 2 |

## Grid Assignment

Three independent grid slots, each with a mix slider (0–5):

- **Density** — scatter/absorption coefficient. Controls opacity and smoke shape.
- **Temperature** — blackbody emission colour in Kelvin. Drives fire colour.
- **Flames** — combustion emission intensity. Adds extra glow.

Any combination works. Empty fields are skipped. Use **Discover Grids** to auto-scan and populate.

## Requirements

- **Windows 11**
- **Visual Studio 2022** with **C++ Clang-cl** (Individual Components)
- **CMake 3.20+**
- **Nuke 17** (tested with 17.0v1)
- **vcpkg** with OpenVDB 12

## Build

```bat
cd C:\
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg && bootstrap-vcpkg.bat
vcpkg install openvdb:x64-windows
```

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

## Install

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
5. Click **Discover Grids** to auto-populate
6. Select a **Scene Preset** or adjust manually

## License

MIT

---

*Created by Marten Blumen · OpenVDB 12.0 · clang-cl 19 · C++20 · Nuke NDK 17*
