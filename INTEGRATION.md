# NeuralVDB Integration — Applied Changes

All changes have been applied to `VDBRenderIop.cpp` and `VDBRenderIop.h`.
This document records what was changed and why, for code review.

Build without LibTorch: identical to v2.1 — all neural code is behind
`#ifdef VDBRENDER_HAS_NEURAL` (set by CMakeLists.txt when `-DVDBRENDER_NEURAL=ON`).
Without the define, `ctx.sampleDensity()` inlines directly to
`BoxSampler::sample()` with zero overhead.

## Files Modified

| File | Change | Lines |
|------|--------|-------|
| `VDBRenderIop.h` | `MarchCtx::sampleDensity()`/`sampleShadow()` abstraction, `_neural` members | +68 |
| `VDBRenderIop.cpp` | All density/shadow BoxSampler replaced, Neural tab, .nvdb loading | +130 (2104→2234) |
| `CMakeLists.txt` | Optional `VDBRENDER_NEURAL` flag + LibTorch linking | +30 |

## Files Added

| File | Purpose |
|------|---------|
| `NeuralDecoder.h` | Header-only neural decoder, `#ifdef`-guarded |
| `build_neural.bat` | Build script with LibTorch auto-detection |
| `setup_neural_branch.bat` | Git branch setup (copies all files, commits) |
| `nvdb_encoder.py` | Python: VDB → NVDB training script |
| `nvdb_validate.py` | Python: compression quality validation |
| `test_synthetic.py` | Python: end-to-end test (no real VDB needed) |
| `requirements.txt` | Python dependencies |

## Sampling Abstraction

The key architectural change: `MarchCtx` now has two inline methods
that dispatch to either BoxSampler or the neural decoder:

```cpp
ctx.sampleDensity(iP)  // replaces BoxSampler::sample(acc, iP)
ctx.sampleShadow(iP)   // replaces BoxSampler::sample(shAcc, iP)
```

When `VDBRENDER_HAS_NEURAL` is **not defined**, these compile to direct
BoxSampler calls — zero overhead, the compiler inlines them away.

When neural mode is active (`_neuralMode && neuralDec != nullptr`),
they route to the neural decoder's topology classifier + value regressor.

Temperature, flame, velocity, and colour grids stay on their own
dedicated OpenVDB accessors, unchanged from v2.1.

## BoxSampler Replacements

**17 calls replaced** (density/shadow → `ctx.sampleDensity`/`sampleShadow`):

| Function | What | Count |
|----------|------|-------|
| `marchRay` (Lit) | density, shadow, env shadow, bounce density, bounce shadow, HDDA adaptive, AABB adaptive | 7 |
| `marchRayExplosion` | density, shadow, env shadow, bounce density, bounce shadow, HDDA adaptive, AABB adaptive | 7 |
| `marchRayDensity` | density | 1 |
| `doDeepEngine` | density + shadow (with `#ifdef` neural dispatch) | 2 |

**7 calls unchanged** (temperature/flame/velocity/colour — not neurally compressed):

- `BoxSampler::sample(*tAcc, iP)` × 3 sites
- `BoxSampler::sample(*fAcc, iP)` × 2 sites
- `BoxSampler::sample(*ctx.velAcc, iP)` × 1 site
- `BoxSampler::sample(*ctx.colorAcc, iPC)` × 1 site

## _validate .nvdb Detection

When the file path ends in `.nvdb`:
1. `NeuralDecoder::load()` reads header + TorchScript models
2. Upper VDB tree extracted → assigned to `_floatGrid`
3. `VolumeRayIntersector` built from upper tree → HDDA still works
4. BBox computed → viewport preview still works
5. Info knobs updated (compression ratio, PSNR)
6. `_neuralMode = true` → MarchCtx routes to neural decoder

When the file ends in `.vdb`: standard loading, `_neuralMode = false`.

## Deep Engine

`doDeepEngine` creates its own accessors (not MarchCtx), so neural
dispatch uses explicit `#ifdef` blocks:
- Density: `_neural->sampleDensity(iP)` when `_neuralMode`, else `dAcc->getValue()`
- Shadow: `_neural->sampleDensity(li)` when `_neuralMode`, else `la.getValue()`

## Structural Integrity

```
VDBRenderIop.cpp:  { 453  } 453   balanced
                   ( 2194 ) 2194  balanced
                   #if 9  #endif 9  balanced

VDBRenderIop.h:    { 35  } 35   balanced
                   #if 3  #endif 3  balanced

NeuralDecoder.h:   { 56  } 56   balanced
                   #if 1  #endif 1  balanced
```
