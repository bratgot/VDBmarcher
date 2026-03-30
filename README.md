# VDBRender

**OpenVDB Volume Ray Marcher for Nuke 17**

A volume renderer that brings VDB files directly into Nuke's 2D compositing pipeline. Supports smoke, fire, explosions, clouds, and particle point clouds with physically-based lighting.

Created by Marten Blumen

---

## Features

**Rendering**
- Ray marching with HDDA empty-space skipping
- Adaptive step size for quality/speed balance
- Deep output for volumetric compositing
- Render region for fast iteration
- Proxy resolution modes (75%, 50%, 25%)
- Stochastic multi-sample anti-aliasing

**Shading & Colour**
- Lit, greyscale, heat, cool, blackbody, custom gradient, and explosion modes
- CIE 1931 blackbody (accurate 500K–40000K, Kang et al. 2002)
- Chromatic extinction (per-channel σt for Rayleigh-like scattering)
- Cd (colour) grid support for scatter albedo

**Phase Functions**
- Dual-lobe Henyey-Greenstein with configurable forward/backward lobes
- Approximate Mie (Jendersie & d'Eon, SIGGRAPH 2023) for clouds and fog
- Powder effect (Schneider & Vos 2015, Horizon Zero Dawn clouds)

**Scattering**
- Analytical multiple scattering (Wrenninge 2015) — zero extra rays
- Gradient-normal Lambertian blend for surface-like shading

**Lighting**
- Auto-detected directional and point lights from Nuke's Scene node
- HDRI environment maps via EnvironLight nodes
- L2 spherical harmonic environment lighting (10–20× faster for clouds)
- Virtual directional lights extracted from HDRI peaks
- ReSTIR weighted reservoir sampling for environment lighting
- Configurable sky/studio light rigs with presets

**Shadows**
- HDDA shadow rays (skips empty VDB tiles)
- Optional transmittance cache (precomputed per-light, 5–10× faster)
- Half/quarter resolution shadow cache options

**Procedural Detail**
- fBm Perlin noise overlay — adds micro-detail beyond VDB resolution
- Configurable scale, strength, octaves, roughness

**VDB Support**
- Density, temperature, flame, velocity, and colour grids
- Auto-sequence detection for numbered VDB files
- Dual VDB layer blending
- Point cloud rendering (PointDataGrid with splatting)
- Motion blur via velocity grid

**AOV Outputs**
- Density, emission, shadow, depth, motion vectors
- Per-light contribution (up to 4 lights)
- Albedo and normal passes (for downstream denoisers)

**Viewport**
- Interactive 3D bounding box and point cloud preview
- Viewport lighting preview
- Configurable point density and size

---

## Requirements

- **Nuke 17.0v1** or later
- **Windows x64**
- **OpenVDB** (via vcpkg)

---

## Building from Source

### Prerequisites

```powershell
# Install vcpkg dependencies
vcpkg install openvdb:x64-windows
```

### Build

```powershell
cmake -B build -DNUKE_ROOT="C:/Program Files/Nuke17.0v1" -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

### Package for Distribution

```powershell
cmake --build build --config Release --target dist
```

Creates `dist/VDBRender_v3/` ready to zip and share.

### Optional: NeuralVDB Support

Neural compressed volumes (10–100× smaller .nvdb files):

```powershell
cmake -B build -DNUKE_ROOT="C:/Program Files/Nuke17.0v1" -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DVDBRENDER_NEURAL=ON -DLIBTORCH_PATH=C:/libtorch
cmake --build build --config Release
```

---

## Installation

1. Copy the `VDBRender_v3` folder to `%USERPROFILE%\.nuke\plugins\` and rename it to `VDBRender`

   Your layout should be:
   ```
   %USERPROFILE%\.nuke\plugins\VDBRender\nuke17\VDBRender.dll
   %USERPROFILE%\.nuke\plugins\VDBRender\nuke17\init.py
   %USERPROFILE%\.nuke\plugins\VDBRender\nuke17\menu.py
   %USERPROFILE%\.nuke\plugins\VDBRender\nuke17\lib\openvdb.dll
   %USERPROFILE%\.nuke\plugins\VDBRender\nuke17\lib\tbb12.dll
   ...
   ```

2. Add this line to `%USERPROFILE%\.nuke\init.py` (create the file if it doesn't exist):
   ```python
   nuke.pluginAddPath('./plugins/VDBRender/nuke17')
   ```

3. Restart Nuke. **VDBRender** appears in the **VDB** menu.

> **Note:** Runtime DLLs live in `lib/` so Nuke doesn't try to load them as plugins. Do not move them into the parent folder.

---

## Usage

1. Create a **VDBRender** node from the VDB menu
2. Connect a **Camera** to input 1
3. Connect a **Scene** node (with lights and optionally an Axis for volume transform) to input 2
4. Set the **VDB File** path and click **Discover Grids**
5. Choose a **Scene Preset** or configure shading manually

### Tips

- **Clouds:** Enable Shadow Cache (Quality tab) and set Env Mode to "SH + Virtual Lights" (Lighting tab) for 10–20× faster rendering
- **Fire/Explosions:** Use the Explosion colour scheme — it evaluates emission before density for fire-first ordering
- **Sequences:** Enable "Auto Sequence" to convert numbered filenames to `####` padding automatically
- **Performance:** Start with a lower Quality value and Proxy mode for fast iteration, then increase for final renders

---

## Inputs

| Input | Label | Description |
|-------|-------|-------------|
| 0 | bg | Background plate (sets output resolution) |
| 1 | cam | Camera |
| 2 | scn | Scene (lights, EnvironLight for HDRI, Axis for volume transform) |

---

## References

- Wrenninge, M. (2015). *Art-Directable Multiple Volumetric Scattering*
- Schneider, A. & Vos, N. (2015). *The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn*
- Jendersie, J. & d'Eon, E. (2023). *An Approximate Mie Scattering Function for Fog and Cloud Rendering*. SIGGRAPH 2023
- Kang, B. et al. (2002). *Design of Advanced Color Temperature Control System for HDTV Applications*

---

## License

See [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt) for third-party library licenses.
