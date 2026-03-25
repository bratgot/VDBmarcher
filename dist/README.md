# NeuralVDB Integration for VDBRender

Neural compressed volume support for VDBRender. Adds `.nvdb` file loading alongside standard `.vdb` — same node, same lighting, same deep output, same everything. Neural volumes are 10-100x smaller with minimal quality loss.

## How It Works

NeuralVDB replaces the lower leaf nodes of the VDB tree with compact neural networks (two small MLPs: one for topology classification, one for value regression). The upper tree is kept intact, so **HDDA empty-space skipping still works**. At render time, `MarchCtx::sampleDensity()` dispatches to either `BoxSampler::sample()` (standard `.vdb`) or the neural decoder (`.nvdb`) — everything above the sampling layer is identical.

## Files

| File | What it does |
|------|-------------|
| `NeuralDecoder.h` | Self-contained neural decoder — header-only, `#ifdef`-guarded |
| `VDBRenderIop.h` | Your header with neural additions (replaces existing) |
| `CMakeLists.txt` | Updated build with optional LibTorch dependency |
| `INTEGRATION.md` | Exact find→replace changes needed in VDBRenderIop.cpp |
| `build_neural.bat` | Build script with auto-detect |
| `nvdb_encoder.py` | Python training script: `.vdb` → `.nvdb` |
| `nvdb_validate.py` | Quality validation (PSNR, error distribution) |
| `test_synthetic.py` | End-to-end test without real VDB data |

## Integration Steps

### 1. Copy files into your project

```
C:\dev\VDBmarcher\
    VDBRenderIop.h      ← replace with the new version
    VDBRenderIop.cpp     ← apply changes from INTEGRATION.md
    NeuralDecoder.h      ← new file, place alongside .h/.cpp
    CMakeLists.txt       ← replace with the new version
```

### 2. Apply code changes to VDBRenderIop.cpp

Follow `INTEGRATION.md` — it has exact find→replace blocks for every change. The changes are:

1. **Knobs**: Add Neural tab after Quality tab
2. **_validate**: Detect `.nvdb`, load via NeuralDecoder instead of openvdb::io::File
3. **makeMarchCtx**: Set neural decoder pointer when in neural mode
4. **March functions**: Replace `BoxSampler::sample(acc,iP)` → `ctx.sampleDensity(iP)` (mechanical find/replace, ~12 sites)
5. **Hash/help**: Minor additions

### 3. Download LibTorch

From https://pytorch.org → Get Started → C++ → cxx11 ABI → extract to `C:\libtorch`

### 4. Build

```bat
build_neural.bat
```

Or manually:
```bat
cmake .. -G "NMake Makefiles" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake ^
    -DNUKE_ROOT="C:/Program Files/Nuke17.0v1" ^
    -DVDBRENDER_NEURAL=ON ^
    -DLIBTORCH_PATH=C:/libtorch
```

**Without LibTorch**: Set `-DVDBRENDER_NEURAL=OFF` (default). The plugin builds exactly as before — all neural code is `#ifdef`-guarded and compiles to zero overhead.

## Encoding Workflow

```bash
# Single frame
python nvdb_encoder.py --input explosion.vdb --output explosion.nvdb

# Animated sequence with warm-starting
python nvdb_encoder.py \
    --input sim/smoke.%04d.vdb \
    --output compressed/smoke.%04d.nvdb \
    --frame-range 1 100 --warm-start

# Higher quality (bigger networks)
python nvdb_encoder.py \
    --input cloud.vdb --output cloud.nvdb \
    --value-hidden 256 --value-layers 6 --value-epochs 500
```

Then in Nuke: load the `.nvdb` file in VDBRender's File knob. Everything else works automatically.

## Architecture

```
.nvdb file
   │
   ├── Upper VDB tree (coarse topology, HDDA-compatible)
   ├── Topology MLP (TorchScript, ~10KB)
   └── Value MLP (TorchScript, ~50KB)
         │
         ▼
   NeuralDecoder::sampleDensity(indexPos)
         │
         ▼  (replaces BoxSampler::sample at leaf level)
         │
   MarchCtx::sampleDensity(iP)  ← dispatches VDB or neural
         │
         ▼  (everything below is unchanged)
         │
   VDBRender march functions
   ├── Lit mode (Beer-Lambert + HG phase)
   ├── Explosion mode (emission + in-scatter)
   ├── Ramp modes (greyscale/heat/cool/blackbody)
   ├── Shadow rays → ctx.sampleShadow()
   ├── Multi-scatter bounces
   ├── Environment map
   ├── Deep output
   └── AOV passes
```

## What Neural Mode Preserves

Everything. The neural decoder only replaces the **density sampling** at the leaf level. All of these work identically in neural mode:

- All 7 render modes (Lit, Greyscale, Heat, Cool, Blackbody, Custom Gradient, Explosion)
- All 9 scene presets
- Sun/sky (Preetham) + studio lighting + mix controls
- Henyey-Greenstein phase function
- Shadow rays and self-shadowing
- Multiple scattering (1-4 bounces)
- Environment map (HDRI)
- Deep output (DeepOutputPlane)
- AOV passes (density, emission, shadow, depth)
- HDDA empty-space skipping (VolumeRayIntersector)
- Adaptive step size
- Proxy rendering
- 3D viewport (bounding box + point cloud)
- Motion blur (velocity grid — standard VDB, not neurally compressed)
- Axis transforms from scene input

## What's Not Neurally Compressed (v1)

- Temperature grids — still loaded from standard VDB accessors
- Flame grids — same
- Velocity grids — same
- Colour grids — same

These can be added to the `.nvdb` format in v2 by extending the encoder to train separate networks per grid type.

## Performance

- **Without neural** (`VDBRENDER_NEURAL=OFF`): Zero overhead. `sampleDensity()` inlines to `BoxSampler::sample()`.
- **With neural, loading .vdb**: Same as before — neural code path not triggered.
- **With neural, loading .nvdb**: Per-sample neural inference. Slower per-sample than BoxSampler, but the volumes are 10-100x smaller so they fit in memory where standard VDB might not.
- **CUDA inference**: Significant speedup on GPU. Set the CUDA Inference checkbox in the Neural tab.

## Python Requirements

```
pip install torch numpy tqdm pyopenvdb
```
