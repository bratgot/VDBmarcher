# VDBRender

OpenVDB volume ray marcher for Nuke 14–17. Physically-based volume rendering with multi-scatter lighting, deep output, environment lighting, motion blur, and AOV passes.

Created by **Marten Blumen**

## Features

- **Physically-based volume rendering** — Beer-Lambert transmittance with Henyey-Greenstein phase function
- **HDDA empty-space skipping** — VolumeRayIntersector skips inactive tree nodes, 5-20x faster on sparse volumes
- **Trilinear interpolation** — BoxSampler for smooth density, shadow, and emission lookups
- **Multiple scattering** — 1-4 bounce approximation with 6-26 sample directions per bounce
- **Environment lighting** — latlong HDRI sampled with shadow rays from multiple directions
- **Deep output** — depth-sorted RGBA slabs for deep compositing via DeepMerge
- **Motion blur** — velocity grid ray-origin offset across configurable shutter interval
- **AOV passes** — density, emission, shadow, and depth as separate output layers
- **Adaptive step size** — automatic 2-4x speedup on sparse volumes
- **Proxy rendering** — 1/4, 1/2, 3/4 downscale for interactive preview
- **Vec3 colour grids** — direct RGB from Houdini Cd/color grids
- **Scene presets** — one-click setups for smoke, cloud, fire, explosion, fog, dust, steam
- **3D viewport** — bounding box wireframe and density point cloud preview

## Inputs

| Input | Type | Description |
|-------|------|-------------|
| `bg` | Iop | Optional background plate. Sets output resolution. Composited under the volume. |
| `cam` | CameraOp | Required. Provides view matrix and focal length for ray generation. |
| `scn` | Any Op | Scene node with lights and Axis transforms. Recursively walked. |
| `env` | Iop | Latlong HDRI for environment lighting. |

## Supported Grids

| Grid | Type | Field Names |
|------|------|-------------|
| Density | float | density, smoke, soot |
| Temperature | float | temperature, heat, temp |
| Flames | float | flame, flames, fire, fuel, burn |
| Velocity | vec3 | vel, v, velocity |
| Colour | vec3 | Cd, color, colour, rgb, albedo |

**Discover Grids** scans the VDB file and auto-populates all fields.

## Render Modes

| Mode | Description |
|------|-------------|
| Lit | Physically-based scatter with shadow rays and phase function |
| Greyscale | Density mapped to luminance |
| Heat | Black → red → yellow → white |
| Cool | Black → blue → cyan → white |
| Blackbody | Planckian locus temperature-to-colour |
| Custom Gradient | User two-colour ramp |
| Explosion | Lit smoke with self-luminous fire emission |

## AOV Outputs

| Pass | Layer | Channels |
|------|-------|----------|
| Density | vdb_density | Greyscale RGB |
| Emission | vdb_emission | Colour RGB |
| Shadow | vdb_shadow | Greyscale RGB |
| Depth | vdb_depth | Single channel (red) |

## Building

### Requirements

- Windows 11
- Nuke 14, 15, 16, or 17 (NDK headers)
- OpenVDB 12 via vcpkg
- clang-cl (Visual Studio 2022)
- CMake

### Build All Versions

Open an **x64 Native Tools Command Prompt** and run:

```bat
cd C:\dev\VDBmarcher
build_all.bat
```

This auto-detects all installed Nuke versions and builds a DLL for each. Output goes to:

```
%USERPROFILE%\.nuke\
    menu.py
    plugins\VDBRender\
        nuke14\VDBRender.dll
        nuke15\VDBRender.dll
        nuke16\VDBRender.dll
        nuke17\VDBRender.dll
```

The `menu.py` auto-detects the running Nuke version and loads the matching DLL.

### Build Single Version

```bat
cd C:\dev\VDBmarcher
rmdir /s /q build && mkdir build && cd build

cmake .. -G "NMake Makefiles" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_C_COMPILER=clang-cl ^
    -DCMAKE_CXX_COMPILER=clang-cl ^
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DNUKE_ROOT="C:/Program Files/Nuke17.0v1"

nmake
copy /Y VDBRender.dll %USERPROFILE%\.nuke\plugins\
```

For Nuke 14–15, add `-DCMAKE_CXX_STANDARD=17`.

### Install

**Multi-version:** Run `build_all.bat` — it copies everything automatically.

**Single version:** Copy `VDBRender.dll` to `~/.nuke/plugins/` and `menu.py` to `~/.nuke/`.

## Multi-Volume Compositing

Each volume gets its own VDBRender node with independent settings. Use deep output for depth-correct compositing:

```
Camera ──┬── VDBRender (smoke.vdb) ──┐
         │                            ├── DeepMerge ── DeepToImage
         ├── VDBRender (fire.vdb)  ──┘
```

## Nuke Compatibility

| Nuke | C++ Standard | Status |
|------|-------------|--------|
| 14.x | C++17 | Supported |
| 15.x | C++17 | Supported |
| 16.x | C++20 | Supported |
| 17.x | C++20 | Primary target |

Same source code across all versions. The DDImage API surface used (Iop, DeepOp, CameraOp, LightOp, AxisOp, knobs) is stable from Nuke 14 through 17.

## Architecture

- Single node, dual `Iop` + `DeepOp` inheritance
- OpenVDB headers included before DDImage to avoid `foreach` macro collision
- VolumeRayIntersector master built at validate time, shallow-copied per thread
- Environment map cached to 128×64 float buffer in `_open()` on the main thread
- 4 always-visible inputs matching ScanlineRender convention

## References

- Museth (2013) — VDB: High-Resolution Sparse Volumes, ACM TOG 32(3)
- Henyey & Greenstein (1941) — Diffuse radiation in the galaxy, ApJ 93
- Fong et al. (2017) — Production Volume Rendering, SIGGRAPH Course
- Novák et al. (2018) — Monte Carlo Methods for Volumetric Light Transport, CGF 37(2)

## License

Apache 2.0
