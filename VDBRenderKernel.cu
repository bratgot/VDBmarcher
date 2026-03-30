// VDBRenderKernel.cu — CUDA primary ray march, Step 1: density-only
// VDBRender v3.5
// Created by Marten Blumen
//
// This file compiles as CUDA C++ (.cu). It must NOT include any Nuke NDK
// headers — only NanoVDB, CUDA runtime, and our own shared header.

#include "VDBRenderKernel.cuh"

#include <nanovdb/NanoVDB.h>
#include <nanovdb/util/HDDA.h>
#include <nanovdb/util/Ray.h>
#include <nanovdb/util/SampleFromVoxels.h>

#include <cuda_runtime.h>
#include <math_constants.h>
#include <cstdio>

// ── Device helpers ────────────────────────────────────────────────────────────

__device__ __forceinline__
float3 operator+(float3 a, float3 b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
__device__ __forceinline__
float3 operator*(float s, float3 v)  { return {s*v.x, s*v.y, s*v.z}; }
__device__ __forceinline__
float  dot3(float3 a, float3 b)      { return a.x*b.x + a.y*b.y + a.z*b.z; }
__device__ __forceinline__
float  len3(float3 v)                { return sqrtf(dot3(v,v)); }
__device__ __forceinline__
float3 norm3(float3 v) { float l=len3(v); return l>1e-8f ? (1.f/l)*v : float3{0,0,1}; }

// ── AABB ray intersection ─────────────────────────────────────────────────────

__device__ bool aabbIntersect(
    float3 ro, float3 rd,
    float bminX, float bminY, float bminZ,
    float bmaxX, float bmaxY, float bmaxZ,
    float& tMin, float& tMax)
{
    float t0=0.f, t1=1e30f;
    float bmin[3]={bminX,bminY,bminZ};
    float bmax[3]={bmaxX,bmaxY,bmaxZ};
    float ro_[3]={ro.x,ro.y,ro.z};
    float rd_[3]={rd.x,rd.y,rd.z};
    for(int a=0;a<3;++a){
        float inv = fabsf(rd_[a])>1e-8f ? 1.f/rd_[a] : 1e30f;
        float ta=(bmin[a]-ro_[a])*inv, tb=(bmax[a]-ro_[a])*inv;
        if(ta>tb){ float tmp=ta;ta=tb;tb=tmp; }
        t0=fmaxf(t0,ta); t1=fminf(t1,tb);
    }
    tMin=t0; tMax=t1;
    return t0<t1 && t1>0.f;
}

// ── Primary kernel ────────────────────────────────────────────────────────────
// Step 1: density-only — accumulates transmittance, outputs greyscale density
// as R=G=B=alpha (same as the CPU "density" render mode).

__global__ void densityMarchKernel(
    const nanovdb::FloatGrid* __restrict__ densGrid,
    GpuCamParams    cam,
    GpuBBox         bbox,
    GpuShadingParams sp,
    float*          outRGBA,   // W×H×4, row-major
    int W, int H)
{
    const int px = blockIdx.x * blockDim.x + threadIdx.x;
    const int py = blockIdx.y * blockDim.y + threadIdx.y;
    if (px >= W || py >= H) return;

    // ── Build ray ─────────────────────────────────────────────────────────────
    const float ndcX = (float)px / (float)W * 2.f - 1.f;
    const float ndcY = (float)py / (float)H * 2.f - 1.f;
    const float rcx  = ndcX * cam.halfW;
    const float rcy  = ndcY * cam.halfW * (float)H / (float)W;
    const float rcz  = -1.f;

    // Camera-space to world-space (cam.rot is column-major)
    float3 rd;
    rd.x = cam.rot[0][0]*rcx + cam.rot[1][0]*rcy + cam.rot[2][0]*rcz;
    rd.y = cam.rot[0][1]*rcx + cam.rot[1][1]*rcy + cam.rot[2][1]*rcz;
    rd.z = cam.rot[0][2]*rcx + cam.rot[1][2]*rcy + cam.rot[2][2]*rcz;
    rd = norm3(rd);

    float3 ro = {cam.ox, cam.oy, cam.oz};

    // ── AABB test ─────────────────────────────────────────────────────────────
    float tMin, tMax;
    if (!aabbIntersect(ro, rd,
            bbox.minX, bbox.minY, bbox.minZ,
            bbox.maxX, bbox.maxY, bbox.maxZ,
            tMin, tMax)) {
        const int idx = (py * W + px) * 4;
        outRGBA[idx]=outRGBA[idx+1]=outRGBA[idx+2]=outRGBA[idx+3]=0.f;
        return;
    }
    tMin = fmaxf(tMin, 0.f);

    // ── Ray march ────────────────────────────────────────────────────────────
    // NanoVDB trilinear sampler (thread-safe, read-only accessor)
    auto acc = densGrid->getAccessor();
    using Sampler = nanovdb::SampleFromVoxels<
        nanovdb::FloatGrid::TreeType, 1, false>;
    const auto& xf = densGrid->map();

    float T    = 1.f;                       // transmittance
    float step = sp.stepSize;
    float t    = tMin + step * 0.1f;        // slight offset to avoid bbox edge artefact

    while (t < tMax && T > sp.earlyOutTransmittance) {
        // World position
        float3 wp = { ro.x + t*rd.x, ro.y + t*rd.y, ro.z + t*rd.z };

        // World → index (NanoVDB inverse map)
        nanovdb::Vec3f iP = xf.applyInverseMap(
            nanovdb::Vec3f(wp.x, wp.y, wp.z));

        // Trilinear sample
        float density = Sampler(acc, iP) * sp.densityMix;
        if (density < 0.f) density = 0.f;

        // Beer-Lambert transmittance update
        T *= expf(-density * sp.extinction * step);

        t += step;
    }

    const float alpha = 1.f - T;

    // Output: greyscale density (R=G=B=alpha, same as CPU density mode)
    const int idx = (py * W + px) * 4;
    outRGBA[idx+0] = alpha;
    outRGBA[idx+1] = alpha;
    outRGBA[idx+2] = alpha;
    outRGBA[idx+3] = alpha;
}

// ── Host launch wrapper ───────────────────────────────────────────────────────

extern "C" void launchDensityKernel(
    const void*             devDensGrid,
    const GpuCamParams&     cam,
    const GpuBBox&          bbox,
    const GpuShadingParams& shading,
    float*                  outRGBA,
    int W, int H,
    cudaStream_t            stream)
{
    dim3 block(16, 16);
    dim3 grid((W + block.x - 1) / block.x,
              (H + block.y - 1) / block.y);

    densityMarchKernel<<<grid, block, 0, stream>>>(
        reinterpret_cast<const nanovdb::FloatGrid*>(devDensGrid),
        cam, bbox, shading,
        outRGBA, W, H);

    // Error check (visible in Nuke's terminal)
    cudaError_t err = cudaGetLastError();
    if (err != cudaSuccess)
        std::fprintf(stderr, "[VDBRender CUDA] kernel error: %s\n",
                     cudaGetErrorString(err));
}
