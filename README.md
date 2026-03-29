# VDBRender

OpenVDB volume ray marcher for Nuke 17. Physically-based single and multiple scatter rendering with deep output, HDRI environment lighting, procedural noise, and NeuralVDB support.

Created by Marten Blumen · [Nukepedia](https://www.nukepedia.com/tools/plugins/3d/vdb-render/)

---

## What's new in v3

- **Dual-lobe HG + approximate Mie** phase functions — forward/backward lobes, silver-lining on clouds, correct fog scattering
- **Powder effect** (Schneider & Vos 2015) — interior brightening for dense fireballs, zero extra rays
- **Analytical multiple scattering** (Wrenninge et al. 2017) — replaces brute-force bounce rays at 100× lower cost
- **Procedural fBm detail noise** — density perturbation at render time, world-space, no UV setup
- **Gradient-normal blend** — sculpted cloud edges, enabled in Cumulus Cloud preset
- **Chromatic extinction** — per-channel σt for Rayleigh-like depth colour shifts
- **HDDA shadow rays** — always active, skips empty tiles on every shadow ray
- **Transmittance cache** — precomputed FloatGrid per directional light, O(1) shadow lookup (5–10× speedup)
- **SH environment lighting** — HDRI projected to L2 spherical harmonics at load time, 6-dir HDDA shadow sampling at render time (10–20× faster for clouds)
- **Virtual directional lights** — brightest HDRI peaks extracted as shadowed directional lights, benefit from transmittance cache
- **CIE 1931 blackbody** — accurate 500K–40000K, replaces limited polynomial approximation
- **Distribution build** — `cmake --build --target dist` produces a ready-to-zip folder with all runtime DLLs

---

## Features

- Physically-based ray marching (Beer–Lambert, HDDA empty-space skipping, adaptive step size)
- Dual-lobe Henyey-Greenstein and approximate Mie (Jendersie & d'Eon 2023) phase functions
- Powder effect, analytical multiple scattering, chromatic extinction, ray jitter
- Procedural fBm detail noise (world-space, per-render)
- HDDA + precomputed transmittance cache shadow system
- SH environment lighting with virtual directional lights
- Sun/sky (Preetham) and studio 3-point lighting rigs with mix controls
- Emission: CIE 1931 blackbody temperature, flame grid, custom gradient
- Velocity motion blur
- Deep output and AOVs — Density, Emission, Shadow, Depth
- 9 scene presets: Thin Smoke, Dense Smoke, Fog/Mist, Cumulus Cloud, Fire, Explosion, Pyroclastic, Dust Storm, Steam
- Auto grid detection and frame sequence handling
- Viewport preview — point cloud + bounding box
- NeuralVDB support (optional, requires LibTorch)

---

## Inputs

| Input | Label | Description |
|---|---|---|
| 0 | `bg` | Background plate — sets output resolution |
| 1 | `cam` | Camera |
| 2 | `scn` | Scene — pipe lights, Axis, and EnvironLight into a Scene node |

---

## Quick start

1. Create a **VDBRender** node from the VDB menu
2. Connect a **Camera** to input 1 and a **Scene** node to input 2
3. Set the **VDB File** path and click **Discover Grids**
4. Choose a **Scene Preset** — Explosion or Cumulus Cloud are good starting points
5. For clouds: enable **Shadow Cache** (Quality tab) and set **Env Mode** to *SH + Virtual Lights* (Lighting tab)

---

## Build — Windows

**Requirements**
- Visual Studio 2022 (x64)
- CMake 3.20+
- Nuke 17.0v1
- OpenVDB via vcpkg: `vcpkg install openvdb:x64-windows`

Open *x64 Native Tools Command Prompt for VS 2022*:

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_TOOLCHAIN_FILE="C:\vcpkg\scripts\buildsystems\vcpkg.cmake" ^
    -DCMAKE_PREFIX_PATH="C:\vcpkg\installed\x64-windows" ^
    -DNUKE_ROOT="C:\Program Files\Nuke17.0v1"

cmake --build build --config Release -j %NUMBER_OF_PROCESSORS%
cmake --install build --config Release
```

Installs to `%USERPROFILE%\.nuke\plugins\VDBRender\`.

**Build a distribution zip:**

```bat
cmake --build build --config Release --target dist
```

Produces `dist\VDBRender_v3\` with the DLL, menu script, all runtime DLLs, INSTALL.txt, and THIRD_PARTY_LICENSES.txt. Rename the folder to `VDBRender` and drop it in `.nuke\plugins\`.

**NeuralVDB (optional — requires LibTorch):**

```bat
cmake -B build ... -DVDBRENDER_NEURAL=ON -DLIBTORCH_PATH="C:\libtorch"
```

---

## Installation (from distribution zip)

1. Rename `VDBRender_v3` to `VDBRender`
2. Copy to `%USERPROFILE%\.nuke\plugins\`
3. Add to `%USERPROFILE%\.nuke\menu.py`:
   ```python
   import VDBRender.VDBRender_menu
   ```
4. Restart Nuke

---

## Performance tips

| Setting | Location | Effect |
|---|---|---|
| Shadow Cache | Quality tab | Precomputes transmittance per directional light — O(1) shadow lookup. Use Half resolution for production. |
| Cache Resolution | Quality tab | Full = best quality. Half = recommended. Quarter = fastest. |
| Env Mode | Lighting tab | *SH + Virtual Lights* is 10–20× faster than *Uniform dirs* for clouds. |
| Virtual Lights | Lighting tab | 1–2 peaks from HDRI as shadowed directional lights. Captures sun + bright sky. |
| Proxy | Quality tab | Downscale for interactive preview. Set to Full before final render. |
| Adaptive Step | Quality tab | 2–4× speedup with minimal quality loss on sparse volumes. Leave on. |

---

## References

- Museth (2013) — OpenVDB, HDDA empty-space skipping
- Henyey & Greenstein (1941) — Phase function for volume scattering
- Schneider & Vos — *Real-Time Volumetric Cloudscapes of Horizon Zero Dawn*, SIGGRAPH 2015
- Wrenninge, Fong, Kulla, Habel — *Production Volume Rendering*, SIGGRAPH 2017
- Hillaire — *Physically-Based & Unified Volumetric Rendering in Frostbite*, SIGGRAPH 2015
- Jendersie & d'Eon — *An Approximate Mie Scattering Function for Fog and Cloud Rendering*, SIGGRAPH 2023
- Ramamoorthi & Hanrahan (2001) — Efficient representation for irradiance environment maps
- Kang et al. (2002) — Planckian locus approximation
- Novák et al. (2018) — Null-scattering / residual ratio tracking

---

## License

MIT — see [LICENSE](LICENSE)

See [THIRD_PARTY_LICENSES.txt](THIRD_PARTY_LICENSES.txt) for OpenVDB, TBB, Blosc, zlib, zstd, LZ4, and Imath.
