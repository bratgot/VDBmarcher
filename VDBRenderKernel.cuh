// VDBRenderKernel.cuh — shared structs between CPU host and CUDA device
// VDBRender v3.5 — CUDA primary ray march (Step 1: density-only transmittance)
// Created by Marten Blumen

#pragma once

// ── Camera parameters ─────────────────────────────────────────────────────────
struct GpuCamParams {
    float ox, oy, oz;           // world-space camera origin
    float rot[3][3];            // camera rotation matrix (column-major)
    float halfW;                // half horizontal FOV in world units at z=-1
    int   W, H;                 // frame dimensions
};

// ── AABB ──────────────────────────────────────────────────────────────────────
struct GpuBBox {
    float minX, minY, minZ;
    float maxX, maxY, maxZ;
};

// ── Shading params (density-only for Step 1) ──────────────────────────────────
struct GpuShadingParams {
    float extinction;           // σt
    float stepSize;             // world-space march step
    float earlyOutTransmittance;// stop marching below this T (default 0.005)
    float densityMix;           // scalar multiplier on sampled density
};

// ── Launch wrapper — called from CPU ──────────────────────────────────────────
// outRGBA: device buffer W×H×4 floats (RGBA), caller allocates
#ifdef __cplusplus
#include <cstdint>
struct nanovdb_float_grid_t; // forward — actual type in kernel TU

extern "C" void launchDensityKernel(
    const void*            devDensGrid,   // nanovdb::FloatGrid* on device
    const GpuCamParams&    cam,
    const GpuBBox&         bbox,
    const GpuShadingParams& shading,
    float*                 outRGBA,       // device buffer
    int W, int H,
    cudaStream_t           stream
);
#endif
