# VDBRender — OpenVDB Ray Marcher for Nuke

A Nuke plugin that ray-marches OpenVDB density grids directly in the compositor. Reads `.vdb` files natively — no conversion needed.

## Features

- **Direct .vdb loading** — reads OpenVDB float grids (density, temperature, etc.)
- **VDB sequences** — auto-detects frame numbers, or use `####`/`%04d` patterns
- **Single-scatter lighting** — directional light with shadow rays
- **3D viewport bounding box** — green wireframe in 3D viewer to help aim the camera
- **2D overlay** — optional wireframe overlay on rendered output
- **Format control** — choose output resolution independently from project settings
- **Frame offset** — shift sequence timing with a simple knob

## Requirements

- **Windows 11**
- **Visual Studio 2022** with **C++ Clang-cl** compiler (Individual Components)
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

OpenVDB 12 requires C++20 features that MSVC doesn't fully support. Install Clang-cl through:

**Visual Studio Installer → Modify → Individual Components → search "Clang"**
- Tick **C++ Clang Compiler for Windows**
- Tick **C++ Clang-cl for v143 build tools**

### 3. Build the plugin

Open an **x64 Native Tools Command Prompt for VS 2022**, then:

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

```bat
mkdir %USERPROFILE%\.nuke\plugins

copy /Y build\VDBRender.dll          %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\openvdb.dll   %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\tbb12.dll     %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\zlib1.dll     %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\blosc.dll     %USERPROFILE%\.nuke\plugins\
copy /Y C:\vcpkg\installed\x64-windows\bin\Imath-3_2.dll %USERPROFILE%\.nuke\plugins\
```

Copy `menu.py` into `%USERPROFILE%\.nuke\` (or merge with your existing one).

## Usage

1. Launch Nuke → **Nodes → VDB → VDBRender**
2. Connect a **Camera** to the input
3. Set **Output Format** (e.g. HD_1080)
4. Point **VDB File** at your `.vdb` file
5. Aim the camera at the volume (use the 3D viewer bounding box as a guide)

### Sequences

Just pick any file from a numbered sequence:
```
dust_impact_0099.vdb
```
The node auto-detects `0099` as the frame number and replaces it with the current timeline frame. Use **Frame Offset** to shift timing.

You can also use explicit patterns:
```
smoke.####.vdb
smoke.%04d.vdb
```

### Knobs

| Knob | Description |
|------|-------------|
| **VDB File** | Path to .vdb file or sequence |
| **Grid Name** | Name of the density grid (default: `density`) |
| **Frame Offset** | Shift sequence timing |
| **Step Size** | Ray march step in world units. Smaller = more detail, slower |
| **Extinction** | How quickly light is absorbed. Higher = denser |
| **Scattering** | How much light bounces. Higher = brighter |
| **Light Direction** | Direction vector (normalised internally) |
| **Light Color** | Color and intensity of the light |
| **3D Display** | Off / Bounding Box / Bbox + Points (Low/Med/High) — density point cloud in 3D viewport |

### Tips

- If you see black, try **pulling the camera back** and increasing **Extinction** and **Scattering**
- Use the **3D viewport** with "Bbox + Points" to see the density shape and aim your camera
- For large volumes, use a bigger **Step Size** (0.5–2.0) for fast preview, then reduce for final
- **Light Color** values above 1.0 act as intensity multipliers
- The point cloud uses additive blending — denser areas glow brighter

## Technical Notes

- Built with **clang-cl** (Clang 19 with MSVC ABI) — needed because OpenVDB 12 uses C++20 features unsupported by MSVC 19.44
- OpenVDB headers must be included **before** Nuke's DDImage headers to avoid a `#define foreach` macro collision in `ChannelSet.h`
- The node is an `Iop` (2D image operator) that accepts a `CameraOp` input via `test_input()` override
- Camera data is cached in `_validate()` since `CameraOp` is not thread-safe for `engine()` calls
- Uses `Foundry` platform macros (`FN_SHARED_IMPORT/EXPORT`) defined via CMake for clang-cl compatibility

## License

MIT
