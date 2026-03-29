# VDBRender

OpenVDB volume ray marcher for Nuke 17. Physically-based single and multiple scatter rendering with deep output, environment lighting, and NeuralVDB support.

Created by Marten Blumen

---

## Features

- **Ray marching** — HDDA empty-space skipping via OpenVDB `VolumeRayIntersector`, adaptive step size, Beer-Lambert transmittance
- **Shading V2** — Dual-lobe Henyey-Greenstein, powder effect, analytical multiple scattering, chromatic extinction, ray jitter
- **Lighting** — Directional and point lights via Nuke scene input, environment map (HDRI), procedural sky rig (Preetham), studio 3-point rig
- **Emission** — Blackbody temperature grid, flame grid, fire self-absorption
- **Deep output** — Depth-sorted RGBA slabs for compositing over CG
- **AOV passes** — Density, emission, shadow, depth
- **Motion blur** — Velocity grid ray-origin offset across shutter interval
- **Vec3 colour grid** — Per-voxel colour (Cd) override
- **NeuralVDB** — Optional neural compressed volume support via LibTorch (10–100× smaller .nvdb files)
- **Viewport** — Live point cloud and bounding box preview in Nuke's 3D viewer

---

## Shading V2

The **Shading V2** tab adds production-grade shading techniques with zero render cost overhead on most features.

### Dual-lobe Henyey-Greenstein

Replaces the single-lobe phase function with a weighted blend of two lobes — forward scatter and backward scatter. Gives backlit smoke its silver-lining rim and explosion fireballs their characteristic backlit corona.

| Knob | Description | Typical values |
|---|---|---|
| `G Forward` | Forward-scatter lobe asymmetry | Smoke: 0.4 · Cloud: 0.8 · Explosion: 0.85 |
| `G Backward` | Backward-scatter lobe asymmetry | Smoke: −0.15 · Cloud: −0.1 · Explosion: −0.25 |
| `Lobe Mix` | Blend weight (1.0 = pure forward) | 0.65–0.85 for most volumes |

### Powder Effect

Interior brightening from multiple forward-scattering (Schneider & Vos, Horizon Zero Dawn, SIGGRAPH 2015). Transforms flat explosion renders into glowing fireballs with a lit interior. Zero extra rays.

| Value | Result |
|---|---|
| 0 | Off |
| 2 | Natural smoke |
| 4–6 | Dense explosion fireball |
| 10 | Maximum |

### Analytical Multiple Scattering

Two-parameter approximation to the infinite-bounce diffusion solution (Wrenninge et al., SIGGRAPH 2017). Replaces the brute-force bounce rays at ~100× lower cost with equivalent quality for dense media. **Scatter Tint** adds a subtle warm or cool bias to the bounced light contribution.

### Chromatic Extinction

Per-channel extinction coefficient (σt per wavelength). Real smoke scatters blue wavelengths more than red — set `Extinction B > Extinction R` for a Rayleigh-like depth colour shift. Shadow rays remain greyscale for performance.

### Ray Jitter

Per-pixel deterministic hash offset on the march start position. Eliminates the concentric banding (wood-grain artifact) that appears at low quality settings. Zero render cost.

### Gradient Mix

Blends the HG phase function with a density-gradient Lambertian term. Gives clouds sculpted, three-dimensional billowing edges. Off by default — adds 6 grid lookups per march step when enabled. Recommended for clouds only.

---

## Inputs

| Input | Label | Description |
|---|---|---|
| 0 | `bg` | Background plate — sets output resolution |
| 1 | `cam` | Camera |
| 2 | `scn` | Scene — pipe lights, an Axis, and optionally an EnvironLight into a Scene node |

---

## Quick start

1. Create a **VDBRender** node from the VDB menu
2. Connect a **Camera** to input 1
3. Connect a **Scene** node (with lights) to input 2
4. Set the **VDB File** path to your `.vdb` file
5. Click **Discover Grids** to auto-detect density, temperature, flame, and velocity grids
6. Choose a **Scene Preset** — Explosion or Cumulus Cloud are good starting points
7. Adjust the **Shading V2** tab for look development

---

## Build — Windows

**Requirements**
- Visual Studio 2022 (x64)
- CMake 3.20+
- Nuke 17.0v1
- OpenVDB via vcpkg: `vcpkg install openvdb:x64-windows`

**Configure and build**

Open *x64 Native Tools Command Prompt for VS 2022*, then:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
    -DCMAKE_PREFIX_PATH="C:\vcpkg\installed\x64-windows" ^
    -DNUKE_ROOT="C:\Program Files\Nuke17.0v1"

cmake --build build --config Release -j %NUMBER_OF_PROCESSORS%
cmake --install build --config Release
```

Output installs to `%USERPROFILE%\.nuke\plugins\VDBRender\nuke17\VDBRender.dll`.

**NeuralVDB build** (optional — requires LibTorch)

```bat
cmake -B build ... -DVDBRENDER_NEURAL=ON -DLIBTORCH_PATH="C:\libtorch"
cmake --build build --config Release -j %NUMBER_OF_PROCESSORS%
```

---

## Installation

1. Copy `VDBRender_menu.py` contents into `~/.nuke/menu.py` (or import it)
2. Copy the required runtime DLLs alongside `VDBRender.dll`:

```
openvdb.dll    Imath-3_2.dll    tbb12.dll
blosc.dll      zlib1.dll        zstd.dll    lz4.dll
```

These are found in `C:\vcpkg\installed\x64-windows\bin\`.

---

## NeuralVDB

Neural compressed volumes reduce `.vdb` file sizes by 10–100× using two small MLPs (topology classifier + value regressor with Fourier positional encoding).

**Encode a VDB file:**

```bash
pip install -r requirements.txt
python nvdb_encoder.py --input smoke.vdb --output smoke.nvdb
```

**Validate compression quality:**

```bash
python nvdb_validate.py --original smoke.vdb --compressed smoke.nvdb
```

Load the resulting `.nvdb` file in VDBRender exactly like a standard `.vdb`. All lighting, shading, and deep output work identically.

---

## References

- Museth (2013) — OpenVDB / HDDA empty-space skipping
- Henyey & Greenstein (1941) — Phase function for volume scattering
- Schneider & Vos — *The Real-Time Volumetric Cloudscapes of Horizon Zero Dawn*, SIGGRAPH 2015 — powder effect, dual-lobe HG
- Wrenninge, Fong, Kulla, Habel — *Production Volume Rendering*, SIGGRAPH 2017 — analytical multiple scattering, RTE derivation
- Hillaire — *Physically-Based & Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015 — chromatic extinction, transmittance volumes
- Novák et al. (2018) — Null-scattering / residual ratio tracking

---

## License

See `THIRD_PARTY_LICENSES.txt` for OpenVDB, LibTorch, and other dependencies.
