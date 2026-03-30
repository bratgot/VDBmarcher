// GpuGridCache.h — NanoVDB conversion and GPU upload helpers
// VDBRender v3.5
// Created by Marten Blumen

#pragma once

#ifdef VDBRENDER_HAS_CUDA

#include <nanovdb/NanoVDB.h>
#include <nanovdb/util/OpenToNanoVDB.h>  // nanovdb::openToNanoVDB()
#include <nanovdb/util/CudaDeviceBuffer.h>

#include <openvdb/openvdb.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <memory>

// ── Holds a NanoVDB grid on both host and device ──────────────────────────────
struct GpuFloatGrid {
    nanovdb::GridHandle<nanovdb::CudaDeviceBuffer> handle;
    void* devPtr = nullptr;   // raw device pointer (FloatGrid*)

    bool valid() const { return devPtr != nullptr; }

    // Convert an OpenVDB FloatGrid and upload to device
    bool upload(const openvdb::FloatGrid& ovdbGrid) {
        try {
            handle = nanovdb::openToNanoVDB<nanovdb::CudaDeviceBuffer>(ovdbGrid);
            handle.deviceUpload(); // async — sync before kernel launch
            devPtr = handle.deviceGrid<float>();
            return devPtr != nullptr;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[VDBRender CUDA] grid upload failed: %s\n", e.what());
            devPtr = nullptr;
            return false;
        }
    }

    void free() {
        devPtr = nullptr;
        handle = {};
    }
};

// ── Device RGBA output buffer ─────────────────────────────────────────────────
struct GpuOutputBuffer {
    float* dev  = nullptr;   // device buffer (W×H×4 floats)
    float* host = nullptr;   // pinned host buffer for fast DMA
    int    W = 0, H = 0;

    bool allocate(int w, int h) {
        free();
        W = w; H = h;
        const size_t bytes = (size_t)W * H * 4 * sizeof(float);
        cudaError_t e1 = cudaMalloc(&dev, bytes);
        cudaError_t e2 = cudaMallocHost(&host, bytes);
        if (e1 != cudaSuccess || e2 != cudaSuccess) {
            std::fprintf(stderr, "[VDBRender CUDA] output buffer alloc failed\n");
            free();
            return false;
        }
        return true;
    }

    // Copy device → pinned host (blocking)
    bool download(cudaStream_t stream = 0) {
        if (!dev || !host) return false;
        const size_t bytes = (size_t)W * H * 4 * sizeof(float);
        cudaError_t err = cudaMemcpyAsync(host, dev, bytes,
                                          cudaMemcpyDeviceToHost, stream);
        if (err != cudaSuccess) {
            std::fprintf(stderr, "[VDBRender CUDA] download failed: %s\n",
                         cudaGetErrorString(err));
            return false;
        }
        cudaStreamSynchronize(stream);
        return true;
    }

    void free() {
        if (dev)  { cudaFree(dev);     dev  = nullptr; }
        if (host) { cudaFreeHost(host);host = nullptr; }
        W = H = 0;
    }

    ~GpuOutputBuffer() { free(); }
};

#endif // VDBRENDER_HAS_CUDA
