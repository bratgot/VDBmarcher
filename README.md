# VDBRender

OpenVDB volume ray marcher for Nuke 17. Physically-based volume rendering with multi-scatter lighting, emission-driven in-scatter, deep output, analytical sun/sky, studio lighting rigs, motion blur, and AOV passes.

Created by **Marten Blumen**

## Features

- **Physically-based volume rendering** — Beer-Lambert transmittance with Henyey-Greenstein phase function
- **Emission-driven in-scatter** — fire and temperature grids illuminate surrounding smoke with warm glow
- **HDDA empty-space skipping** — VolumeRayIntersector skips inactive tree nodes, 5-20x faster on sparse volumes
- **Trilinear interpolation** — BoxSampler for smooth density, shadow, and emission lookups
- **Multiple scattering** — 1-4 bounce approximation with 6-26 sample directions per bounce
- **Analytical sun/sky** — simplified Preetham model with elevation, azimuth, turbidity, and ground bounce
- **Studio lighting rigs** — 3-Point, Dramatic, Soft, Rim Only, and Top Light presets with key/fill/rim controls
- **Light mixing** — Sky Mix and Studio Mix sliders for blending outdoor and studio lighting
- **Environment lighting** — latlong HDRI from Nuke's Environment node via the scene input
- **Deep output** — depth-sorted RGBA slabs for deep compositing via DeepMerge
- **Motion blur** — velocity grid ray-origin offset across configurable shutter interval (beta)
- **AOV passes** — separated density, emission, shadow, and depth output layers
- **Auto sequence** — converts numbered VDB filenames to #### padding automatically
- **Adaptive step size** — automatic 2-4x speedup in thin regions
- **Proxy rendering** — 1/4, 1/2, 3/4 downscale for interactive preview
- **Vec3 colour grids** — direct RGB from Houdini Cd/color grids (beta)
- **Scene presets** — one-click setups for smoke, cloud, fire, explosion, fog, dust, steam
- **3D viewport** — bounding box wireframe and density point cloud preview (no camera required)

## Nuke Compatibility

| Nuke | Status | Reason |
|------|--------|--------|
| 14.x | Not supported | Ships old TBB (`tbb.dll`), incompatible with OpenVDB 12 |
| 15.x | Not supported | Same TBB conflict |
| 16.x | Not supported | Same TBB conflict |
| **17.x** | **Supported** | Ships oneAPI TBB (`tbb12.dll`) |

Nuke 14-16 ship a pre-oneAPI version of TBB that conflicts at process level with the oneAPI TBB required by OpenVDB 12. Future Nuke versions shipping `tbb12.dll` will work automatically.

## Inputs

| Input | Type | Description |
|-------|------|-------------|
| `bg` | Iop | Optional background plate. Sets output resolution. Composited under the volume. |
| `cam` | CameraOp | Required for rendering. Provides view matrix and focal length. |
| `scn` | Any Op | Scene node with lights, Environment, and Axis transforms. Recursively walked. |

Connect lights, an Environment node (with HDRI), and an Axis into a Scene node, then connect the Scene to the `scn` input. All lights and the Environment are auto-detected.

## Supported Grids

| Grid | Type | Field Names |
|------|------|-------------|
| Density | float | density, smoke, soot |
| Temperature | float | temperature, heat, temp |
| Flames | float | flame, flames, fire, fuel, burn |
| Velocity | vec3 | vel, v, velocity (beta) |
| Colour | vec3 | Cd, color, colour, rgb, albedo (beta) |

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
| Explosion | Lit smoke with self-luminous fire, emission-driven in-scatter |

## Lighting

### Light Rig System

Built-in lighting that works out of the box — no scene lights required. Sky and Studio lights have independent presets and Mix sliders for blending.

**Sky Presets** (set sun/sky slider values, then tweak freely):

| Preset | Description |
|--------|-------------|
| Off | Disables sun and sky |
| Custom | Manual slider control |
| Day Sky | Clear noon sun, blue sky dome, ground bounce |
| Golden Hour | Low warm sunset, amber fill |
| Overcast | Soft white dome, no direct sun |
| Blue Hour | Cool post-sunset twilight |
| Night / Moon | Dim cool moonlight |

**Studio Presets** (set key/fill/rim slider values):

| Preset | Description |
|--------|-------------|
| Off | Disables studio lights |
| 3-Point | Key + fill + rim (standard) |
| Dramatic | Strong key, 8% fill, hard rim |
| Soft | Dual wrap keys, gentle rim (beauty) |
| Rim Only | Backlight silhouette |
| Top Light | Overhead, moody |

**Mix controls** blend the two systems: Sky Mix=1 + Studio Mix=0.5 gives full outdoor sun with a subtle studio edge. Both animatable.

### Analytical Sun/Sky Model

Simplified Preetham model. Sun colour shifts from white (noon) through gold to orange-red (horizon) based on elevation and turbidity. Sky dome follows Rayleigh scattering. Ground bounce approximates Lambertian diffuse reflection.

### Environment Map

Connect a Nuke `Environment` node to the scene input. The HDRI and rotation are picked up automatically. The Environment Map section in the Lighting tab provides intensity, rotation offset, and diffuse spread controls.

### Emission-Driven In-Scatter

Fire and temperature grids act as embedded isotropic light sources inside the volume. The emission at each march step is scattered into nearby density using the same scattering coefficient as external lights, normalized by 1/(4π). Fire regions also have self-absorption proportional to emission intensity, preventing unbounded accumulation and building natural alpha in fire-only zones.

## AOV Outputs

| Pass | Layer | Content |
|------|-------|---------|
| Density | vdb_density | Integrated volume opacity (greyscale RGB) |
| Emission | vdb_emission | Self-luminous fire/temperature contribution only (colour RGB) |
| Shadow | vdb_shadow | Transmittance within the volume, black outside (greyscale RGB) |
| Depth | vdb_depth | First-hit camera distance (single channel) |

The emission AOV is cleanly separated from scattered light — it contains only the self-luminous contribution from temperature and flame grids. Smoke-only renders produce a black emission AOV.

## Scene Presets

Each preset configures render mode, shading, emission, quality, and lighting rig:

| Preset | Albedo | Lighting | Notes |
|--------|--------|----------|-------|
| Thin Smoke | 0.75 | Day Sky | Wispy, translucent |
| Dense Smoke | 0.40 | Day Sky | Sooty, dark carbon |
| Fog / Mist | 0.95 | Overcast | Near-lossless water droplets |
| Cumulus Cloud | 0.95 | Day Sky | 3-bounce for bright interiors |
| Fire | 0.40 | Golden Hour | Sooty smoke + emission |
| Explosion | 0.25 | Day Sky | Dense dark smoke + fireball |
| Pyroclastic | 0.20 | Day Sky | Volcanic debris, minimal scatter |
| Dust Storm | 0.75 | Day Sky | Mineral dust, forward scatter |
| Steam | 0.95 | Studio Soft | Water vapour, near-lossless |

All presets have physically valid albedos (scattering ≤ extinction).

## Building

### Requirements

- Windows 11
- Nuke 17 (NDK headers)
- OpenVDB 12 via vcpkg (`vcpkg install openvdb:x64-windows`)
- clang-cl (Visual Studio 2022)
- CMake

### Build

Open an **x64 Native Tools Command Prompt** and run:

```bat
cd C:\dev\VDBmarcher
build_all.bat
```

Auto-detects Nuke installations with `tbb12.dll` and builds for each. Output:

```
%USERPROFILE%\.nuke\
    menu.py              (appended, never overwritten)
    plugins\VDBRender\
        nuke17\VDBRender.dll + dependency DLLs
```

### Manual Build

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
```

### Install

**Automatic:** Run `build_all.bat` — copies everything and appends the menu snippet.

**Manual:** Copy `VDBRender.dll` and dependency DLLs to `~/.nuke/plugins/`, append `VDBRender_menu.py` to your `~/.nuke/menu.py`.

**Dependency DLLs** (must be alongside `VDBRender.dll`):
`openvdb.dll`, `tbb12.dll`, `tbbmalloc.dll`, `Imath-3_2.dll`, `blosc.dll`, `zlib1.dll`, `zstd.dll`, `lz4.dll`

## Performance

Compiled with AVX2 + FMA + fast-math. Additional optimisations:

- **MarchCtx** — grid accessors created once per scanline, not per pixel (~1000× fewer allocations)
- **AABB early-out** — pixels that miss the volume bounding box skip the march entirely
- **Adaptive step** — 4× larger steps in thin regions, 2× in medium, normal in dense
- **Raised transmittance cutoff** — march terminates earlier in opaque regions
- **Shadow ray early-out** — shadow rays terminate 10× sooner when fully occluded

## Multi-Volume Compositing

Each volume gets its own VDBRender node with independent settings. Use deep output for depth-correct compositing:

```
Camera ──┬── VDBRender (smoke.vdb) ──┐
         │                            ├── DeepMerge ── DeepToImage
         ├── VDBRender (fire.vdb)  ──┘
```

## UI Layout

Four tabs: **VDBRender**, **Output**, **Lighting**, **Quality**.

**VDBRender** — VDB file, auto-sequence, format, grids (with Discover), render mode, shading, emission, technical reference.

**Output** — AOV pass toggles, deep samples, motion blur.

**Lighting** — Sky preset + sliders, Studio preset + sliders, mix controls, ambient, environment map, manual override, technical reference.

**Quality** — Quality preset, step resolution, shadows, multi-scatter, adaptive step, proxy, 3D viewport.

## Architecture

- Single node, dual `Iop` + `DeepOp` inheritance
- OpenVDB headers included before DDImage to avoid `foreach` macro collision
- VolumeRayIntersector master built at validate time, shallow-copied per thread
- MarchCtx struct pools grid accessors per scanline for engine thread safety
- Environment map cached to 128×64 float buffer in `_open()` on the main thread
- Light rig generates CachedLight entries identical to scene lights
- 3 always-visible inputs (bg, cam, scn)

## References

- Museth (2013) — VDB: High-Resolution Sparse Volumes, ACM TOG 32(3)
- Henyey & Greenstein (1941) — Diffuse radiation in the galaxy, ApJ 93
- Preetham, Shirley, Smits (1999) — A Practical Analytic Model for Daylight, SIGGRAPH 99
- Fong et al. (2017) — Production Volume Rendering, SIGGRAPH Course
- Novák et al. (2018) — Monte Carlo Methods for Volumetric Light Transport, CGF 37(2)

## License

Apache 2.0 — see `LICENSE`

Bundled dependency licenses — see `THIRD_PARTY_LICENSES.txt`
