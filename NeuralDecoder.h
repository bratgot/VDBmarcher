#pragma once
// NeuralDecoder.h — Neural volume decoder for VDBRender integration
// v2: Decode-to-grid + batched inference optimizations
//
// Two decode modes:
//   1. DECODE-TO-GRID (default, recommended by NeuralVDB paper):
//      At load time, decode all active voxels via batched neural inference
//      into a standard FloatGrid. Rendering then uses BoxSampler at full
//      speed — zero neural overhead per sample.
//
//   2. LIVE NEURAL (fallback, lower memory):
//      Per-sample neural inference during rendering. Slower but the decoded
//      grid doesn't need to fit in memory alongside the original.
//
// Usage in _validate:
//   _neural->load(path);
//   _floatGrid = _neural->decodeToGrid(4096); // batched decode
//   // rendering uses BoxSampler on decoded grid — no neural dispatch needed

#ifdef VDBRENDER_HAS_NEURAL

#include <torch/torch.h>

#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iostream>
#include <chrono>

namespace neural {

// ─── Positional Encoding ───────────────────────────────────────────
class PosEncoder {
public:
    explicit PosEncoder(int L = 6) : _L(L) {
        _bands = torch::zeros({L});
        auto a = _bands.accessor<float, 1>();
        for (int i = 0; i < L; i++) a[i] = std::pow(2.0f, (float)i);
    }
    torch::Tensor encode(const torch::Tensor& xyz) const {
        auto dev = xyz.device();
        auto bands = _bands.to(dev);
        std::vector<torch::Tensor> feat;
        feat.reserve(1 + 2 * _L);
        feat.push_back(xyz);
        for (int i = 0; i < _L; i++) {
            float f = bands[i].item<float>();
            feat.push_back(torch::sin(f * (float)M_PI * xyz));
            feat.push_back(torch::cos(f * (float)M_PI * xyz));
        }
        return torch::cat(feat, 1);
    }
    int dim() const { return 3 + 6 * _L; }
private:
    int _L;
    torch::Tensor _bands;
};

// ─── NVDB File Format ──────────────────────────────────────────────
static constexpr uint32_t NVDB_MAGIC = 0x4E564442;

#pragma pack(push, 1)
struct NVDBHeader {
    uint32_t magic = NVDB_MAGIC, verMaj = 1, verMin = 0, flags = 0;
    float voxelSize[3] = {};
    int32_t bboxMin[3] = {}, bboxMax[3] = {};
    uint64_t origBytes = 0, compBytes = 0;
    float psnr = 0;
    uint32_t gridClass = 0, valType = 0, numFreq = 6;
    uint32_t topoH = 64, topoL = 3, valH = 128, valL = 4;
    uint8_t reserved[20] = {};
};
struct SectHead {
    uint32_t magic = NVDB_MAGIC, type = 0;
    uint64_t size = 0;
};
#pragma pack(pop)

// ─── Network Modules (reconstructed from header config) ────────────
struct TopoClassifier : torch::nn::Module {
    torch::nn::Sequential layers{nullptr};
    TopoClassifier(int in, int h, int n) {
        layers = torch::nn::Sequential();
        layers->push_back(torch::nn::Linear(in, h));
        layers->push_back(torch::nn::ReLU());
        for (int i = 1; i < n - 1; i++) {
            layers->push_back(torch::nn::Linear(h, h));
            layers->push_back(torch::nn::ReLU());
        }
        layers->push_back(torch::nn::Linear(h, 1));
        layers->push_back(torch::nn::Sigmoid());
        register_module("layers", layers);
    }
    torch::Tensor forward(torch::Tensor x) { return layers->forward(x).squeeze(-1); }
};

struct ValRegressor : torch::nn::Module {
    torch::nn::Sequential layers{nullptr};
    ValRegressor(int in, int h, int n) {
        layers = torch::nn::Sequential();
        layers->push_back(torch::nn::Linear(in, h));
        layers->push_back(torch::nn::ReLU());
        for (int i = 1; i < n - 1; i++) {
            layers->push_back(torch::nn::Linear(h, h));
            layers->push_back(torch::nn::ReLU());
        }
        layers->push_back(torch::nn::Linear(h, 1));
        register_module("layers", layers);
    }
    torch::Tensor forward(torch::Tensor x) { return layers->forward(x).squeeze(-1); }
};

// ─── Neural Decoder ────────────────────────────────────────────────
class NeuralDecoder {
public:
    NeuralDecoder() {}

    bool load(const std::string& path, bool useCuda = false) {
        _dev = (useCuda && torch::cuda::is_available())
             ? torch::Device(torch::kCUDA) : torch::kCPU;

        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return false;
        ifs.read((char*)&_hdr, sizeof(_hdr));
        if (_hdr.magic != NVDB_MAGIC) return false;

        // Read section index
        struct Sect { uint32_t type; uint64_t off, sz; };
        std::vector<Sect> sects;
        uint64_t pos = sizeof(NVDBHeader);
        while (ifs.good()) {
            SectHead sh; ifs.read((char*)&sh, sizeof(sh));
            if (!ifs.good() || sh.magic != NVDB_MAGIC) break;
            sects.push_back({sh.type, pos + sizeof(SectHead), sh.size});
            if (sh.type == 0xFF) break;
            ifs.seekg(sh.size, std::ios::cur);
            pos += sizeof(SectHead) + sh.size;
        }

        auto readSect = [&](uint32_t t) -> std::vector<uint8_t> {
            for (auto& s : sects) if (s.type == t) {
                std::vector<uint8_t> d(s.sz);
                ifs.clear(); ifs.seekg(s.off);
                ifs.read((char*)d.data(), s.sz);
                return d;
            }
            return {};
        };

        // Upper VDB tree (section 0x01)
        auto treeData = readSect(0x01);
        if (!treeData.empty()) {
            std::string tmp = path + ".upper.tmp.vdb";
            { std::ofstream o(tmp, std::ios::binary);
              o.write((char*)treeData.data(), treeData.size()); }
            try {
                openvdb::io::File f(tmp); f.open();
                auto grids = f.getGrids();
                if (grids && !grids->empty())
                    _upperGrid = openvdb::gridPtrCast<openvdb::FloatGrid>((*grids)[0]);
                f.close();
            } catch (...) {}
            std::remove(tmp.c_str());
        }
        if (!_upperGrid) return false;

        _xform = _upperGrid->transformPtr();
        _bbox = _upperGrid->evalActiveVoxelBoundingBox();

        // Pre-compute normalisation bounds (cached, not per-sample)
        auto bmin = _xform->indexToWorld(_bbox.min().asVec3d());
        auto bmax = _xform->indexToWorld((_bbox.max() + openvdb::Coord(1)).asVec3d());
        _normCenter = (bmin + bmax) * 0.5;
        _normExtent = (bmax - bmin) * 0.5;
        for (int i = 0; i < 3; i++)
            if (std::abs(_normExtent[i]) < 1e-6) _normExtent[i] = 1.0;

        // Create encoder
        _encoder = std::make_unique<PosEncoder>(_hdr.numFreq);
        int encDim = _encoder->dim();

        // Load parameters into reconstructed networks
        auto loadParams = [&](const std::vector<uint8_t>& data, torch::nn::Module& mod) -> bool {
            if (data.empty()) return false;
            try {
                const char* ptr = (const char*)data.data();
                int32_t count; std::memcpy(&count, ptr, 4); ptr += 4;
                auto params = mod.parameters();
                if ((int)params.size() != count) {
                    std::cerr << "[NeuralVDB] param count mismatch: file=" << count
                              << " model=" << params.size() << "\n";
                    return false;
                }
                for (auto& p : params) {
                    int32_t ndim; std::memcpy(&ndim, ptr, 4); ptr += 4;
                    std::vector<int64_t> shape(ndim);
                    for (int d = 0; d < ndim; d++) {
                        std::memcpy(&shape[d], ptr, 8); ptr += 8;
                    }
                    int64_t nbytes; std::memcpy(&nbytes, ptr, 8); ptr += 8;
                    auto t = torch::from_blob((void*)ptr, shape, torch::kFloat32).clone().to(_dev);
                    p.set_data(t);
                    ptr += nbytes;
                }
                return true;
            } catch (const std::exception& e) {
                std::cerr << "[NeuralVDB] param load: " << e.what() << "\n";
                return false;
            }
        };

        // Topology model (section 0x02)
        auto topoData = readSect(0x02);
        if (!topoData.empty()) {
            _topoNet = std::make_shared<TopoClassifier>(encDim, _hdr.topoH, _hdr.topoL);
            _topoNet->to(_dev);
            if (loadParams(topoData, *_topoNet)) {
                _topoNet->eval();
                _hasTopo = true;
            }
        }

        // Value model (section 0x03)
        auto valData = readSect(0x03);
        if (!valData.empty()) {
            _valNet = std::make_shared<ValRegressor>(encDim, _hdr.valH, _hdr.valL);
            _valNet->to(_dev);
            if (loadParams(valData, *_valNet)) {
                _valNet->eval();
                _hasVal = true;
            }
        }

        ifs.close();
        _loaded = true;

        if (_hdr.compBytes > 0)
            _ratio = (float)_hdr.origBytes / (float)_hdr.compBytes;

        std::cout << "[NeuralVDB] Loaded: ratio=" << _ratio
                  << "x  psnr=" << _hdr.psnr << "dB  dev=" << _dev << "\n";
        return true;
    }

    void unload() {
        _upperGrid.reset(); _xform.reset();
        _topoNet.reset(); _valNet.reset();
        _hasTopo = _hasVal = _loaded = false;
    }

    bool loaded() const { return _loaded; }

    // ═══════════════════════════════════════════════════════════════
    //  OPTIMIZATION 1: Decode-to-Grid
    //  Recommended by NeuralVDB paper (Kim et al. 2024):
    //  "online random access via inference is too slow for real-time
    //   applications — decode neural→regular VDB first, then render"
    //
    //  One-time batched decode of all active voxels → FloatGrid.
    //  After this, rendering uses BoxSampler at full speed.
    //  batchSize controls GPU memory vs throughput tradeoff.
    // ═══════════════════════════════════════════════════════════════

    openvdb::FloatGrid::Ptr decodeToGrid(int batchSize = 4096) const {
        if (!_loaded || !_hasVal) return nullptr;

        auto t0 = std::chrono::high_resolution_clock::now();

        // Collect all active voxel coordinates from upper tree
        std::vector<openvdb::Coord> activeCoords;
        activeCoords.reserve(_upperGrid->activeVoxelCount());
        for (auto iter = _upperGrid->cbeginValueOn(); iter; ++iter)
            activeCoords.push_back(iter.getCoord());

        int N = (int)activeCoords.size();
        std::cout << "[NeuralVDB] Decoding " << N << " active voxels";
        if (_dev.is_cuda()) std::cout << " (CUDA)";
        std::cout << "..." << std::flush;

        // Create output grid with same transform
        auto outGrid = openvdb::FloatGrid::create(0.0f);
        outGrid->setTransform(_xform->copy());
        outGrid->setGridClass(openvdb::GRID_FOG_VOLUME);
        auto outAcc = outGrid->getAccessor();

        torch::NoGradGuard ng;

        // Process in batches — single tensor creation + forward pass per batch
        for (int start = 0; start < N; start += batchSize) {
            int end = std::min(start + batchSize, N);
            int batchN = end - start;

            // Build normalised coordinate tensor
            auto coords = torch::zeros({batchN, 3}, torch::kFloat32);
            auto ca = coords.accessor<float, 2>();
            for (int i = 0; i < batchN; i++) {
                auto worldP = _xform->indexToWorld(activeCoords[start + i].asVec3d());
                ca[i][0] = (float)((worldP[0] - _normCenter[0]) / _normExtent[0]);
                ca[i][1] = (float)((worldP[1] - _normCenter[1]) / _normExtent[1]);
                ca[i][2] = (float)((worldP[2] - _normCenter[2]) / _normExtent[2]);
            }
            coords = coords.to(_dev);

            // Encode — one call for entire batch
            auto encoded = _encoder->encode(coords);

            // Topology filter
            std::vector<bool> topoMask(batchN, true);
            if (_hasTopo && _topoNet) {
                auto topoPred = _topoNet->forward(encoded);
                auto topoCpu = topoPred.cpu();
                auto topoA = topoCpu.accessor<float, 1>();
                for (int i = 0; i < batchN; i++)
                    topoMask[i] = (topoA[i] >= 0.5f);
            }

            // Value prediction — one forward pass for entire batch
            auto valPred = _valNet->forward(encoded);
            auto valCpu = valPred.cpu();
            auto valA = valCpu.accessor<float, 1>();
           

            // Write decoded values to output grid
            for (int i = 0; i < batchN; i++) {
                if (!topoMask[i]) continue;
                float v = std::clamp(valA[i], 0.0f, 1.0f);
                if (v > 1e-6f)
                    outAcc.setValue(activeCoords[start + i], v);
            }

            // Progress indicator
            if ((start / batchSize) % 100 == 0 && start > 0)
                std::cout << "." << std::flush;
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        int64_t decodedCount = outGrid->activeVoxelCount();
        std::cout << " done (" << (int)ms << "ms, "
                  << decodedCount << " voxels, "
                  << (int)(N / (ms / 1000.0)) << " voxels/sec)\n";

        return outGrid;
    }

    // ═══════════════════════════════════════════════════════════════
    //  OPTIMIZATION 2: Batched live sampling
    //  For live neural mode (when decode-to-grid is disabled).
    //  Collects N positions, runs one forward pass instead of N.
    // ═══════════════════════════════════════════════════════════════

    void sampleDensityBatch(const openvdb::Vec3d* indexPositions, float* outValues,
                            int count) const {
        if (!_loaded || !_hasVal || count == 0) {
            for (int i = 0; i < count; i++) outValues[i] = 0.0f;
            return;
        }

        torch::NoGradGuard ng;

        // Filter by upper tree activity, build batch
        std::vector<int> activeIdx;
        activeIdx.reserve(count);
        auto coords = torch::zeros({count, 3}, torch::kFloat32);
        auto ca = coords.accessor<float, 2>();

        for (int i = 0; i < count; i++) {
            outValues[i] = 0.0f;
            openvdb::Coord ijk((int)std::floor(indexPositions[i][0]),
                               (int)std::floor(indexPositions[i][1]),
                               (int)std::floor(indexPositions[i][2]));
            if (!_upperGrid->tree().isValueOn(ijk)) continue;

            auto worldP = _xform->indexToWorld(indexPositions[i]);
            int idx = (int)activeIdx.size();
            ca[idx][0] = (float)((worldP[0] - _normCenter[0]) / _normExtent[0]);
            ca[idx][1] = (float)((worldP[1] - _normCenter[1]) / _normExtent[1]);
            ca[idx][2] = (float)((worldP[2] - _normCenter[2]) / _normExtent[2]);
            activeIdx.push_back(i);
        }

        if (activeIdx.empty()) return;

        int batchN = (int)activeIdx.size();
        coords = coords.slice(0, 0, batchN).to(_dev);
        auto encoded = _encoder->encode(coords);

        // Topology filter
        std::vector<bool> topoMask(batchN, true);
        if (_hasTopo && _topoNet) {
            auto topoPred = _topoNet->forward(encoded);
            auto topoCpu = topoPred.cpu();
            auto topoA = topoCpu.accessor<float, 1>();

            for (int i = 0; i < batchN; i++)
                topoMask[i] = (topoA[i] >= 0.5f);
        }

        // Value regression — single forward pass
        auto valPred = _valNet->forward(encoded);
        auto valCpu2 = valPred.cpu();
        auto valA = valCpu2.accessor<float, 1>();

        for (int i = 0; i < batchN; i++) {
            if (!topoMask[i]) continue;
            outValues[activeIdx[i]] = std::clamp(valA[i], 0.0f, 1.0f);
        }
    }

    // Single-point sampling (compatibility — uses batch of 1)
    float sampleDensity(const openvdb::Vec3d& indexPos) const {
        float result = 0.0f;
        sampleDensityBatch(&indexPos, &result, 1);
        return result;
    }

    // Accessors
    openvdb::FloatGrid::Ptr         upperGrid()  const { return _upperGrid; }
    openvdb::math::Transform::Ptr   transform()  const { return _xform; }
    openvdb::CoordBBox              activeBBox() const { return _bbox; }
    float                           ratio()      const { return _ratio; }
    float                           psnr()       const { return _hdr.psnr; }
    const NVDBHeader&               header()     const { return _hdr; }

private:
    bool _loaded = false, _hasTopo = false, _hasVal = false;
    NVDBHeader _hdr;
    torch::Device _dev = torch::kCPU;

    openvdb::FloatGrid::Ptr _upperGrid;
    openvdb::math::Transform::Ptr _xform;
    openvdb::CoordBBox _bbox;

    // Pre-computed normalisation (avoids recomputing per sample)
    openvdb::Vec3d _normCenter, _normExtent;

    // Reconstructed networks (raw parameter serialization, no TorchScript)
    mutable std::shared_ptr<TopoClassifier> _topoNet;
    mutable std::shared_ptr<ValRegressor> _valNet;
    std::unique_ptr<PosEncoder> _encoder;
    float _ratio = 1.0f;
};

} // namespace neural

#endif // VDBRENDER_HAS_NEURAL
