# VDBRender — OpenVDB Ray Marcher for Nuke

A Nuke plugin that ray-marches OpenVDB density grids directly in the compositor.

## Features

- **Direct .vdb loading** with auto-detected frame sequences
- **Multiple render modes** — Lit, Greyscale, Heat, Cool, Blackbody, Custom Gradient
- **Physically-based blackbody** — density-to-temperature color mapping
- **Custom gradient** — user-defined two-color ramp
- **Single-scatter lighting** — directional light with shadow rays
- **Axis transform input** — move/rotate/scale the volume via an Axis node
- **3D viewport preview** — wireframe + density point cloud with selectable color scheme
- **Proxy rendering** — respects Nuke's downres settings
- **Frame offset** for sequence retiming

## Render Modes

| Mode | Description |
|------|-------------|
| **Lit** | Standard ray-marched single-scatter lighting with shadow rays |
| **Greyscale** | Density mapped to luminance (white) |
| **Heat** | Black → Red → Yellow → White ramp |
| **Cool** | Black → Blue → Cyan → White ramp |
| **Blackbody** | Physically-based temperature color. Density maps to Kelvin range. |
| **Custom Gradient** | Two-color ramp between user-defined start and end colors |

The **Blackbody** mode maps density to a temperature range (Temp Min → Temp Max in Kelvin) and evaluates the Planckian locus for accurate incandescent colors. Good values:
- Fire: 500K–3000K
- Explosions: 1000K–8000K
- Stars/plasma: 3000K–15000K

The **Custom Gradient** uses two Color knobs (Gradient Start / Gradient End) and lerps between them based on density.

## Requirements

- **Windows 11**, **Visual Studio 2022** with **Clang-cl**, **CMake 3.20+**
- **Nuke 17**, **vcpkg** with OpenVDB 12

## Build

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

Run `install_vdbrender.bat` or manually copy `VDBRender.dll` plus runtime DLLs (`openvdb.dll`, `tbb12.dll`, `zlib1.dll`, `blosc.dll`, `Imath-3_2.dll`) to `%USERPROFILE%\.nuke\plugins\`.

Copy `menu.py` into `%USERPROFILE%\.nuke\`.

## Usage

1. **Nodes → VDB → VDBRender**
2. Connect a **Camera** to input 0
3. Optionally connect an **Axis** to input 1
4. Set **Output Format**, point at a `.vdb` file
5. Choose a **Render Mode** for 2D output
6. Choose a **Viewport Color** for 3D preview

### Inputs

| Input | Label | Type | Required |
|-------|-------|------|----------|
| 0 | cam | Camera | Yes |
| 1 | axis | Axis/Transform | No |

### Knobs

| Knob | Description |
|------|-------------|
| **Output Format** | Resolution. Respects proxy. |
| **VDB File** | Path or sequence pattern |
| **Grid Name** | Default: `density` |
| **Frame Offset** | Shift sequence timing |
| **Step Size** | 0.5 preview, 0.05 final |
| **Extinction** | Opacity. Higher = denser. |
| **Scattering** | Brightness (Lit mode only) |
| **Light Direction** | For Lit mode |
| **Light Color** | For Lit mode. >1 = intensity boost |
| **Render Mode** | Lit / Greyscale / Heat / Cool / Blackbody / Custom |
| **Temp Min/Max** | Kelvin range for Blackbody mode |
| **Gradient Start/End** | Colors for Custom Gradient mode |
| **Show Bounding Box** | 3D viewport wireframe |
| **Show Point Cloud** | 3D viewport density preview |
| **Point Density** | Low / Medium / High |
| **Point Size** | GL point size |
| **Viewport Color** | Color scheme for 3D points |

## License

MIT
