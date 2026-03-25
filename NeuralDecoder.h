#pragma once
// NeuralDecoder.h — Neural volume decoder for VDBRender integration
// Replaces BoxSampler::sample() at the grid accessor level.
//
// Design: keeps the upper VDB tree for HDDA empty-space skipping,
// replaces lower leaf nodes with compact MLPs. The VolumeRayIntersector
// still works because the upper tree topology is preserved.
//
// Usage in MarchCtx:
//   float d = _neuralMode ? _neural->sampleDensity(iP) : BoxSampler::sample(acc, iP);
//
// Requires LibTorch (C++ distribution, cxx11 ABI)

#ifdef VDBRENDER_HAS_NEURAL

#include <torch/torch.h>
#include <torch/script.h>
#include <openvdb/openvdb.h>
#include <openvdb/tools/Interpolation.h>

#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <cmath>
#include <fstream>
#include <sstream>
#include <iostream>

namespace neural {

// ─── Positional Encoding ───────────────────────────────────────────
// Fourier features for high-frequency detail capture.
// Maps xyz → [xyz, sin(2^k π xyz), cos(2^k π xyz)] for k = 0..L-1
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

// ─── Block Cache (LRU) ────────────────────────────────────────────
// Caches decoded 8^3 voxel blocks keyed by upper-tree node index.
// Prevents redundant inference when multiple rays hit the same region.
struct DecodedBlock {
    openvdb::Coord origin;
    std::vector<float> values;  // 8^3 = 512 floats
    int dim = 8;
};

class BlockCache {
public:
    explicit BlockCache(size_t cap = 256 * 1024 * 1024)
        : _cap(cap) {}

    const DecodedBlock* get(uint64_t key) const {
        std::lock_guard<std::mutex> lk(_mtx);
        auto it = _map.find(key);
        if (it == _map.end()) return nullptr;
        it->second.tick = _tick++;
        return &it->second.blk;
    }

    void put(uint64_t key, DecodedBlock blk) {
        std::lock_guard<std::mutex> lk(_mtx);
        size_t bytes = blk.values.size() * 4 + sizeof(DecodedBlock);
        while (_used + bytes > _cap && !_map.empty()) evictOne();
        _map[key] = {std::move(blk), _tick++};
        _used += bytes;
    }

    void clear() {
        std::lock_guard<std::mutex> lk(_mtx);
        _map.clear(); _used = 0;
    }

private:
    void evictOne() {
        auto oldest = _map.begin();
        for (auto it = _map.begin(); it != _map.end(); ++it)
            if (it->second.tick < oldest->second.tick) oldest = it;
        _used -= oldest->second.blk.values.size() * 4 + sizeof(DecodedBlock);
        _map.erase(oldest);
    }
    struct Entry { DecodedBlock blk; mutable int64_t tick; };
    mutable std::mutex _mtx;
    std::unordered_map<uint64_t, Entry> _map;
    size_t _cap, _used = 0;
    mutable int64_t _tick = 0;
};

// ─── NVDB File Format ──────────────────────────────────────────────
// Binary layout: Header(128) + [SectionHeader(16) + Data]...
static constexpr uint32_t NVDB_MAGIC = 0x4E564442; // "NVDB"

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

// ─── Neural Decoder ────────────────────────────────────────────────
// Single-point sampling interface compatible with VDBRender's march loop.
// Keeps upper VDB tree for HDDA, uses MLPs for leaf-level decode.
class NeuralDecoder {
public:
    NeuralDecoder() : _cache(512ULL * 1024 * 1024) {}

    bool load(const std::string& path, bool useCuda = false) {
        _dev = (useCuda && torch::cuda::is_available())
             ? torch::Device(torch::kCUDA) : torch::kCPU;

        // Read .nvdb header + section offsets
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return false;
        ifs.read((char*)&_hdr, sizeof(_hdr));
        if (_hdr.magic != NVDB_MAGIC) return false;

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

        // Read sections by type
        auto readSect = [&](uint32_t t) -> std::vector<uint8_t> {
            for (auto& s : sects) if (s.type == t) {
                std::vector<uint8_t> d(s.sz);
                ifs.clear(); ifs.seekg(s.off);
                ifs.read((char*)d.data(), s.sz);
                return d;
            }
            return {};
        };

        // Upper VDB tree (section 0x01) → reconstruct OpenVDB grid for HDDA
        auto treeData = readSect(0x01);
        if (!treeData.empty()) {
            // Write to temp file, load via OpenVDB I/O
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

        // Topology model (section 0x02) → TorchScript
        auto topoData = readSect(0x02);
        if (!topoData.empty()) {
            try {
                std::istringstream ss(std::string(topoData.begin(), topoData.end()));
                _topoModel = torch::jit::load(ss, _dev);
                _topoModel.eval();
                _hasTopo = true;
            } catch (const c10::Error& e) {
                std::cerr << "[NeuralVDB] topo load: " << e.what() << "\n";
            }
        }

        // Value model (section 0x03) → TorchScript
        auto valData = readSect(0x03);
        if (!valData.empty()) {
            try {
                std::istringstream ss(std::string(valData.begin(), valData.end()));
                _valModel = torch::jit::load(ss, _dev);
                _valModel.eval();
                _hasVal = true;
            } catch (const c10::Error& e) {
                std::cerr << "[NeuralVDB] val load: " << e.what() << "\n";
            }
        }

        _encoder = std::make_unique<PosEncoder>(_hdr.numFreq);
        ifs.close();
        _loaded = true;

        // Compression stats
        if (_hdr.compBytes > 0)
            _ratio = (float)_hdr.origBytes / (float)_hdr.compBytes;

        std::cout << "[NeuralVDB] Loaded: ratio=" << _ratio
                  << "x  psnr=" << _hdr.psnr << "dB  dev=" << _dev << "\n";
        return true;
    }

    void unload() {
        _upperGrid.reset(); _xform.reset(); _cache.clear();
        _hasTopo = _hasVal = _loaded = false;
    }

    bool loaded() const { return _loaded; }

    // ── Single-point sampling (drop-in for BoxSampler::sample) ──
    // Takes INDEX-space position (same as BoxSampler), returns float value.
    // This is what gets called from VDBRender's march lambdas.
    float sampleDensity(const openvdb::Vec3d& indexPos) const {
        if (!_loaded || !_hasVal) return 0.0f;

        // Quick upper-tree check: is this region active?
        openvdb::Coord ijk((int)std::floor(indexPos[0]),
                           (int)std::floor(indexPos[1]),
                           (int)std::floor(indexPos[2]));
        if (!_upperGrid->tree().isValueOn(ijk)) return 0.0f;

        // Convert index → world → normalised coords for the network
        openvdb::Vec3d worldP = _xform->indexToWorld(indexPos);
        return decodePoint(worldP);
    }

    // Accessors for VDBRender integration
    openvdb::FloatGrid::Ptr         upperGrid()  const { return _upperGrid; }
    openvdb::math::Transform::Ptr   transform()  const { return _xform; }
    openvdb::CoordBBox              activeBBox() const { return _bbox; }
    float                           ratio()      const { return _ratio; }
    float                           psnr()       const { return _hdr.psnr; }
    const NVDBHeader&               header()     const { return _hdr; }

private:
    float decodePoint(const openvdb::Vec3d& worldP) const {
        torch::NoGradGuard ng;

        // Normalise to [-1,1] based on grid bbox
        auto bmin = _xform->indexToWorld(_bbox.min().asVec3d());
        auto bmax = _xform->indexToWorld((_bbox.max() + openvdb::Coord(1)).asVec3d());
        auto center = (bmin + bmax) * 0.5;
        auto extent = (bmax - bmin) * 0.5;
        for (int i = 0; i < 3; i++)
            if (std::abs(extent[i]) < 1e-6) extent[i] = 1.0;

        float nx = (float)((worldP[0] - center[0]) / extent[0]);
        float ny = (float)((worldP[1] - center[1]) / extent[1]);
        float nz = (float)((worldP[2] - center[2]) / extent[2]);

        auto coords = torch::tensor({{nx, ny, nz}}, torch::kFloat32).to(_dev);
        auto encoded = _encoder->encode(coords);

        // Topology check
        if (_hasTopo) {
            auto topo = _topoModel.forward({encoded}).toTensor().squeeze();
            if (topo.item<float>() < 0.5f) return 0.0f;
        }

        // Value regression
        auto val = _valModel.forward({encoded}).toTensor().squeeze();
        return std::clamp(val.item<float>(), 0.0f, 1.0f);
    }

    bool _loaded = false, _hasTopo = false, _hasVal = false;
    NVDBHeader _hdr;
    torch::Device _dev = torch::kCPU;

    openvdb::FloatGrid::Ptr _upperGrid;
    openvdb::math::Transform::Ptr _xform;
    openvdb::CoordBBox _bbox;

   // mutable: forward() is non-const in TorchScript API
    mutable torch::jit::script::Module _topoModel, _valModel;
    std::unique_ptr<PosEncoder> _encoder;
    mutable BlockCache _cache;
    float _ratio = 1.0f;
};

} // namespace neural

#endif // VDBRENDER_HAS_NEURAL
