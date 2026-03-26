// nvdb_encode.cpp — C++ NeuralVDB Encoder
// Reads .vdb via OpenVDB, trains topology + value MLPs, writes .nvdb
// Build with vcpkg OpenVDB + LibTorch (same toolchain as VDBRender)
//
// Usage:
//   nvdb_encode.exe --input smoke.vdb --output smoke.nvdb
//   nvdb_encode.exe --input smoke.####.vdb --output smoke.####.nvdb --start 0 --end 100

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <torch/torch.h>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <random>
#include <sstream>

// ═══════════════════════════════════════════════════════════════════
// NVDB file format (must match NeuralDecoder.h)
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
// Neural network modules
// ═══════════════════════════════════════════════════════════════════

struct PosEncoderImpl : torch::nn::Module {
    int L;
    torch::Tensor bands;

    PosEncoderImpl(int numFreq = 6) : L(numFreq) {
        bands = torch::zeros({L});
        auto a = bands.accessor<float, 1>();
        for (int i = 0; i < L; i++) a[i] = std::pow(2.0f, (float)i);
        bands = register_buffer("bands", bands);
    }

    torch::Tensor forward(torch::Tensor x) {
        std::vector<torch::Tensor> feat;
        feat.push_back(x);
        for (int i = 0; i < L; i++) {
            float f = bands[i].item<float>();
            feat.push_back(torch::sin(f * (float)M_PI * x));
            feat.push_back(torch::cos(f * (float)M_PI * x));
        }
        return torch::cat(feat, 1);
    }

    int dim() const { return 3 + 6 * L; }
};
TORCH_MODULE(PosEncoder);

struct TopoNetImpl : torch::nn::Module {
    torch::nn::Sequential layers{nullptr};

    TopoNetImpl(int inDim, int hidden, int nLayers) {
        layers = torch::nn::Sequential();
        layers->push_back(torch::nn::Linear(inDim, hidden));
        layers->push_back(torch::nn::ReLU());
        for (int i = 1; i < nLayers - 1; i++) {
            layers->push_back(torch::nn::Linear(hidden, hidden));
            layers->push_back(torch::nn::ReLU());
        }
        layers->push_back(torch::nn::Linear(hidden, 1));
        layers->push_back(torch::nn::Sigmoid());
        register_module("layers", layers);
    }

    torch::Tensor forward(torch::Tensor x) {
        return layers->forward(x).squeeze(-1);
    }
};
TORCH_MODULE(TopoNet);

struct ValueNetImpl : torch::nn::Module {
    torch::nn::Sequential layers{nullptr};

    ValueNetImpl(int inDim, int hidden, int nLayers) {
        layers = torch::nn::Sequential();
        layers->push_back(torch::nn::Linear(inDim, hidden));
        layers->push_back(torch::nn::ReLU());
        for (int i = 1; i < nLayers - 1; i++) {
            layers->push_back(torch::nn::Linear(hidden, hidden));
            layers->push_back(torch::nn::ReLU());
        }
        layers->push_back(torch::nn::Linear(hidden, 1));
        register_module("layers", layers);
    }

    torch::Tensor forward(torch::Tensor x) {
        return layers->forward(x).squeeze(-1);
    }
};
TORCH_MODULE(ValueNet);

// ═══════════════════════════════════════════════════════════════════
// VDB data extraction
// ═══════════════════════════════════════════════════════════════════

struct VDBData {
    std::vector<float> coords;    // Nx3 flattened
    std::vector<float> values;    // N
    std::vector<float> inactiveCoords; // Mx3 flattened
    openvdb::Vec3d voxelSize;
    openvdb::Coord bboxMin, bboxMax;
    openvdb::FloatGrid::Ptr grid;
    uint64_t fileBytes;
};

VDBData extractVDB(const std::string& path, const std::string& gridName) {
    VDBData data;

    // File size
    {std::ifstream f(path, std::ios::binary | std::ios::ate);
     data.fileBytes = f.good() ? (uint64_t)f.tellg() : 0;}

    openvdb::io::File file(path);
    file.open();

    // Find grid
    openvdb::GridBase::Ptr base;
    std::string target = gridName.empty() ? "density" : gridName;

    // Try exact name first
    for (auto it = file.beginName(); it != file.endName(); ++it) {
        if (it.gridName() == target) {
            base = file.readGrid(target);
            break;
        }
    }
    // Fallback: first float grid
    if (!base) {
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            auto g = file.readGrid(it.gridName());
            if (g->isType<openvdb::FloatGrid>()) {
                base = g;
                std::cout << "  Using grid: " << it.gridName() << std::endl;
                break;
            }
        }
    }
    file.close();

    if (!base || !base->isType<openvdb::FloatGrid>()) {
        std::cerr << "ERROR: No float grid found in " << path << std::endl;
        return data;
    }

    data.grid = openvdb::gridPtrCast<openvdb::FloatGrid>(base);
    const auto& xf = data.grid->transform();
    auto vs = xf.voxelSize();
    data.voxelSize = vs;

    // Extract active voxels
    auto acc = data.grid->getConstAccessor();
    auto bbox = data.grid->evalActiveVoxelBoundingBox();
    data.bboxMin = bbox.min();
    data.bboxMax = bbox.max();

    // Iterate active values
    for (auto iter = data.grid->cbeginValueOn(); iter; ++iter) {
        auto coord = iter.getCoord();
        auto worldP = xf.indexToWorld(coord.asVec3d());
        data.coords.push_back((float)worldP.x());
        data.coords.push_back((float)worldP.y());
        data.coords.push_back((float)worldP.z());
        data.values.push_back(iter.getValue());
    }

    int N = (int)data.values.size();
    std::cout << "  Active voxels: " << N << std::endl;

    if (N == 0) return data;

    // Value range
    float vMin = *std::min_element(data.values.begin(), data.values.end());
    float vMax = *std::max_element(data.values.begin(), data.values.end());
    std::cout << "  Value range: [" << vMin << ", " << vMax << "]" << std::endl;

    // Sample inactive positions (negative examples for topology classifier)
    std::mt19937 rng(42);
    auto lo = data.bboxMin;
    auto hi = data.bboxMax;
    int pad = 2;
    std::uniform_int_distribution<int> distX(lo.x() - pad, hi.x() + pad);
    std::uniform_int_distribution<int> distY(lo.y() - pad, hi.y() + pad);
    std::uniform_int_distribution<int> distZ(lo.z() - pad, hi.z() + pad);

    int nInactive = std::min(N, 500000); // cap for memory
    data.inactiveCoords.reserve(nInactive * 3);
    int found = 0;
    int attempts = 0;
    while (found < nInactive && attempts < nInactive * 10) {
        openvdb::Coord c(distX(rng), distY(rng), distZ(rng));
        if (!acc.isValueOn(c)) {
            auto wp = xf.indexToWorld(c.asVec3d());
            data.inactiveCoords.push_back((float)wp.x());
            data.inactiveCoords.push_back((float)wp.y());
            data.inactiveCoords.push_back((float)wp.z());
            found++;
        }
        attempts++;
    }
    std::cout << "  Inactive samples: " << found << std::endl;

    return data;
}

// ═══════════════════════════════════════════════════════════════════
// Normalisation
// ═══════════════════════════════════════════════════════════════════

struct NormParams {
    float centerX, centerY, centerZ;
    float extentX, extentY, extentZ;
    float valMin, valRange;
};

NormParams computeNorm(const VDBData& data) {
    NormParams p;
    int N = (int)data.values.size();

    // Coordinate bounds
    float minX = 1e30f, minY = 1e30f, minZ = 1e30f;
    float maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
    for (int i = 0; i < N; i++) {
        float x = data.coords[i * 3 + 0], y = data.coords[i * 3 + 1], z = data.coords[i * 3 + 2];
        if (x < minX) minX = x; if (x > maxX) maxX = x;
        if (y < minY) minY = y; if (y > maxY) maxY = y;
        if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
    }
    p.centerX = (minX + maxX) * 0.5f; p.extentX = std::max((maxX - minX) * 0.5f, 1e-6f);
    p.centerY = (minY + maxY) * 0.5f; p.extentY = std::max((maxY - minY) * 0.5f, 1e-6f);
    p.centerZ = (minZ + maxZ) * 0.5f; p.extentZ = std::max((maxZ - minZ) * 0.5f, 1e-6f);

    // Value range
    p.valMin = *std::min_element(data.values.begin(), data.values.end());
    float vMax = *std::max_element(data.values.begin(), data.values.end());
    p.valRange = std::max(vMax - p.valMin, 1e-8f);

    return p;
}

torch::Tensor normaliseCoords(const float* coords, int N, const NormParams& p) {
    auto t = torch::zeros({N, 3});
    auto a = t.accessor<float, 2>();
    for (int i = 0; i < N; i++) {
        a[i][0] = (coords[i * 3 + 0] - p.centerX) / p.extentX;
        a[i][1] = (coords[i * 3 + 1] - p.centerY) / p.extentY;
        a[i][2] = (coords[i * 3 + 2] - p.centerZ) / p.extentZ;
    }
    return t;
}

// ═══════════════════════════════════════════════════════════════════
// Training
// ═══════════════════════════════════════════════════════════════════

struct TrainConfig {
    int numFreq = 6;
    int topoHidden = 64, topoLayers = 3;
    int valHidden = 128, valLayers = 4;
    int topoEpochs = 50, valEpochs = 200;
    float lr = 1e-3f;
    int batchSize = 4096;
};

float trainTopology(PosEncoder& enc, TopoNet& net,
                    const VDBData& data, const NormParams& norm,
                    const TrainConfig& cfg)
{
    int nPos = (int)data.values.size();
    int nNeg = (int)(data.inactiveCoords.size() / 3);
    int nTotal = nPos + nNeg;

    auto posCoords = normaliseCoords(data.coords.data(), nPos, norm);
    auto negCoords = normaliseCoords(data.inactiveCoords.data(), nNeg, norm);
    auto allCoords = torch::cat({posCoords, negCoords}, 0);
    auto labels = torch::cat({torch::ones({nPos}), torch::zeros({nNeg})});

    // Shuffle
    auto perm = torch::randperm(nTotal);
    allCoords = allCoords.index_select(0, perm);
    labels = labels.index_select(0, perm);

    auto opt = torch::optim::Adam(net->parameters(), cfg.lr);

    float bestAcc = 0;
    for (int ep = 0; ep < cfg.topoEpochs; ep++) {
        net->train();
        float totalLoss = 0;
        int correct = 0, batches = 0;

        for (int start = 0; start < nTotal; start += cfg.batchSize) {
            int end = std::min(start + cfg.batchSize, nTotal);
            auto bc = allCoords.slice(0, start, end);
            auto bl = labels.slice(0, start, end);

            auto encoded = enc->forward(bc);
            auto pred = net->forward(encoded);
            auto loss = torch::binary_cross_entropy(pred, bl);

            opt.zero_grad();
            loss.backward();
            opt.step();

            totalLoss += loss.item<float>();
            correct += ((pred > 0.5f) == (bl > 0.5f)).sum().item<int>();
            batches++;
        }

        float acc = 100.0f * correct / nTotal;
        bestAcc = std::max(bestAcc, acc);

        if ((ep + 1) % 10 == 0 || ep == 0) {
            printf("  Topo epoch %3d/%d: loss=%.4f  acc=%.1f%%\n",
                   ep + 1, cfg.topoEpochs, totalLoss / batches, acc);
        }
    }
    printf("  Best topology accuracy: %.1f%%\n", bestAcc);
    return bestAcc;
}

float trainValues(PosEncoder& enc, ValueNet& net,
                  const VDBData& data, const NormParams& norm,
                  const TrainConfig& cfg)
{
    int N = (int)data.values.size();
    auto coords = normaliseCoords(data.coords.data(), N, norm);

    // Normalise values to [0,1]
    auto values = torch::zeros({N});
    {auto a = values.accessor<float, 1>();
     for (int i = 0; i < N; i++)
         a[i] = (data.values[i] - norm.valMin) / norm.valRange;}

    auto opt = torch::optim::Adam(net->parameters(), cfg.lr);
    auto scheduler = torch::optim::StepLR(opt, 100, 0.5);

    float bestPSNR = 0;
    for (int ep = 0; ep < cfg.valEpochs; ep++) {
        net->train();
        float totalLoss = 0;
        int batches = 0;

        for (int start = 0; start < N; start += cfg.batchSize) {
            int end = std::min(start + cfg.batchSize, N);
            auto bc = coords.slice(0, start, end);
            auto bv = values.slice(0, start, end);

            auto encoded = enc->forward(bc);
            auto pred = net->forward(encoded);
            auto loss = torch::mse_loss(pred, bv);

            opt.zero_grad();
            loss.backward();
            opt.step();

            totalLoss += loss.item<float>();
            batches++;
        }
        scheduler.step();

        float avgLoss = totalLoss / batches;
        float psnr = -10.0f * std::log10(std::max(avgLoss, 1e-10f));
        bestPSNR = std::max(bestPSNR, psnr);

        if ((ep + 1) % 20 == 0 || ep == 0) {
            printf("  Value epoch %3d/%d: MSE=%.6f  PSNR=%.1f dB\n",
                   ep + 1, cfg.valEpochs, avgLoss, psnr);
        }
    }
    printf("  Best PSNR: %.1f dB\n", bestPSNR);
    return bestPSNR;
}

// ═══════════════════════════════════════════════════════════════════
// .nvdb file writing
// ═══════════════════════════════════════════════════════════════════

bool writeNVDB(const std::string& outPath, const VDBData& data,
               PosEncoder& enc, TopoNet& topo, ValueNet& val,
               float psnr, const TrainConfig& cfg)
{
    // Serialize models to TorchScript
    topo->eval();
    val->eval();

    // Serialize parameters as raw tensors (no TorchScript dependency)
    auto saveParams = [](torch::nn::Module& mod) -> std::string {
        std::ostringstream buf;
        auto params = mod.parameters();
        int32_t count = (int32_t)params.size();
        buf.write((const char*)&count, 4);
        for (auto& p : params) {
            auto t = p.contiguous().cpu();
            int32_t ndim = (int32_t)t.dim();
            buf.write((const char*)&ndim, 4);
            for (int d = 0; d < ndim; d++) {
                int64_t s = t.size(d);
                buf.write((const char*)&s, 8);
            }
            int64_t nbytes = (int64_t)(t.numel() * sizeof(float));
            buf.write((const char*)&nbytes, 8);
            buf.write((const char*)t.data_ptr<float>(), nbytes);
        }
        return buf.str();
    };

    std::string topoBytes = saveParams(*topo);
    std::string valBytes = saveParams(*val);
    

    // Serialize upper VDB tree to temp file, read bytes
    std::string treeBytes;
    {
        std::string tmpPath = outPath + ".upper.tmp.vdb";
        openvdb::io::File tmpFile(tmpPath);
        openvdb::GridPtrVec grids;
        grids.push_back(data.grid);
        tmpFile.write(grids);
        tmpFile.close();

        std::ifstream ifs(tmpPath, std::ios::binary | std::ios::ate);
        size_t sz = ifs.tellg();
        ifs.seekg(0);
        treeBytes.resize(sz);
        ifs.read(treeBytes.data(), sz);
        ifs.close();
        std::remove(tmpPath.c_str());
    }

    // Build header
    NVDBHeader hdr;
    hdr.voxelSize[0] = (float)data.voxelSize.x();
    hdr.voxelSize[1] = (float)data.voxelSize.y();
    hdr.voxelSize[2] = (float)data.voxelSize.z();
    hdr.bboxMin[0] = data.bboxMin.x(); hdr.bboxMin[1] = data.bboxMin.y(); hdr.bboxMin[2] = data.bboxMin.z();
    hdr.bboxMax[0] = data.bboxMax.x(); hdr.bboxMax[1] = data.bboxMax.y(); hdr.bboxMax[2] = data.bboxMax.z();
    hdr.origBytes = data.fileBytes;
    hdr.psnr = psnr;
    hdr.numFreq = cfg.numFreq;
    hdr.topoH = cfg.topoHidden; hdr.topoL = cfg.topoLayers;
    hdr.valH = cfg.valHidden; hdr.valL = cfg.valLayers;

    // Compute compressed size
    uint64_t compBytes = sizeof(NVDBHeader)
        + sizeof(SectHead) + treeBytes.size()
        + sizeof(SectHead) + topoBytes.size()
        + sizeof(SectHead) + valBytes.size()
        + sizeof(SectHead); // END
    hdr.compBytes = compBytes;

    // Write file
    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs.is_open()) {
        std::cerr << "ERROR: Cannot open output file: " << outPath << std::endl;
        return false;
    }

    ofs.write((const char*)&hdr, sizeof(hdr));

    auto writeSect = [&](uint32_t type, const void* d, uint64_t sz) {
        SectHead sh;
        sh.type = type;
        sh.size = sz;
        ofs.write((const char*)&sh, sizeof(sh));
        if (sz > 0) ofs.write((const char*)d, sz);
    };

    writeSect(0x01, treeBytes.data(), treeBytes.size());    // UPPER_TREE
    writeSect(0x02, topoBytes.data(), topoBytes.size());    // TOPOLOGY_MODEL
    writeSect(0x03, valBytes.data(), valBytes.size());       // VALUE_MODEL
    writeSect(0xFF, nullptr, 0);                             // END

    ofs.close();

    float ratio = (data.fileBytes > 0) ? (float)data.fileBytes / (float)compBytes : 1.0f;
    printf("\n  Written: %s\n", outPath.c_str());
    printf("  Original:   %llu bytes (%.1f MB)\n", data.fileBytes, data.fileBytes / 1048576.0);
    printf("  Compressed: %llu bytes (%.1f MB)\n", compBytes, compBytes / 1048576.0);
    printf("  Ratio:      %.1fx\n", ratio);
    printf("  PSNR:       %.1f dB\n", psnr);

    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Frame path resolution (matches VDBRender's logic)
// ═══════════════════════════════════════════════════════════════════

std::string resolveFrame(const std::string& pattern, int frame) {
    std::string p = pattern;

    // #### padding
    size_t h = p.find('#');
    if (h != std::string::npos) {
        size_t he = h;
        while (he < p.size() && p[he] == '#') ++he;
        char buf[64];
        std::snprintf(buf, 64, "%0*d", (int)(he - h), frame);
        p.replace(h, he - h, buf);
        return p;
    }

    // %04d padding
    size_t pc = p.find('%');
    if (pc != std::string::npos) {
        char buf[64];
        std::snprintf(buf, 64, p.c_str(), frame);
        return std::string(buf);
    }

    return p;
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char* argv[]) {
    openvdb::initialize();

    std::string inputPath, outputPath, gridName = "density";
    int startFrame = -1, endFrame = -1;
    bool warmStart = false;
    TrainConfig cfg;

    // Parse args
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if ((arg == "--input" || arg == "-i") && i + 1 < argc) inputPath = argv[++i];
        else if ((arg == "--output" || arg == "-o") && i + 1 < argc) outputPath = argv[++i];
        else if (arg == "--grid" && i + 1 < argc) gridName = argv[++i];
        else if (arg == "--start" && i + 1 < argc) startFrame = std::atoi(argv[++i]);
        else if (arg == "--end" && i + 1 < argc) endFrame = std::atoi(argv[++i]);
        else if (arg == "--warm-start") warmStart = true;
        else if (arg == "--topo-epochs" && i + 1 < argc) cfg.topoEpochs = std::atoi(argv[++i]);
        else if (arg == "--value-epochs" && i + 1 < argc) cfg.valEpochs = std::atoi(argv[++i]);
        else if (arg == "--lr" && i + 1 < argc) cfg.lr = (float)std::atof(argv[++i]);
        else if (arg == "--topo-hidden" && i + 1 < argc) cfg.topoHidden = std::atoi(argv[++i]);
        else if (arg == "--value-hidden" && i + 1 < argc) cfg.valHidden = std::atoi(argv[++i]);
        else if (arg == "--freq" && i + 1 < argc) cfg.numFreq = std::atoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            printf("nvdb_encode — NeuralVDB Encoder\n\n");
            printf("Usage:\n");
            printf("  nvdb_encode --input smoke.vdb --output smoke.nvdb\n");
            printf("  nvdb_encode --input smoke.####.vdb --output smoke.####.nvdb --start 0 --end 100\n\n");
            printf("Options:\n");
            printf("  --input/-i        Input .vdb file (use #### for sequences)\n");
            printf("  --output/-o       Output .nvdb file\n");
            printf("  --grid            Grid name (default: density)\n");
            printf("  --start/--end     Frame range for sequences\n");
            printf("  --warm-start      Init each frame from previous frame's weights\n");
            printf("  --topo-epochs     Topology training epochs (default: 50)\n");
            printf("  --value-epochs    Value training epochs (default: 200)\n");
            printf("  --lr              Learning rate (default: 0.001)\n");
            printf("  --topo-hidden     Topology hidden dim (default: 64)\n");
            printf("  --value-hidden    Value hidden dim (default: 128)\n");
            printf("  --freq            Positional encoding frequencies (default: 6)\n");
            return 0;
        }
    }

    if (inputPath.empty() || outputPath.empty()) {
        std::cerr << "Usage: nvdb_encode --input file.vdb --output file.nvdb\n";
        std::cerr << "       nvdb_encode --help for all options\n";
        return 1;
    }

    // Single frame or sequence?
    bool isSequence = (startFrame >= 0 && endFrame >= startFrame);

    if (isSequence) {
        printf("\n============================================\n");
        printf(" NeuralVDB Encoder — Sequence %d to %d\n", startFrame, endFrame);
        printf("============================================\n");

        PosEncoder enc(cfg.numFreq);
        TopoNet topo(enc->dim(), cfg.topoHidden, cfg.topoLayers);
        ValueNet val(enc->dim(), cfg.valHidden, cfg.valLayers);

        for (int frame = startFrame; frame <= endFrame; frame++) {
            std::string inFile = resolveFrame(inputPath, frame);
            std::string outFile = resolveFrame(outputPath, frame);

            printf("\n--- Frame %d ---\n", frame);
            printf("  Input:  %s\n", inFile.c_str());

            // Check file exists
            {std::ifstream chk(inFile);
             if (!chk.good()) { printf("  SKIP: file not found\n"); continue; }}

            auto data = extractVDB(inFile, gridName);
            if (data.values.empty()) { printf("  SKIP: no data\n"); continue; }

            auto norm = computeNorm(data);

            if (!warmStart || frame == startFrame) {
                // Fresh networks
                enc = PosEncoder(cfg.numFreq);
                topo = TopoNet(enc->dim(), cfg.topoHidden, cfg.topoLayers);
                val = ValueNet(enc->dim(), cfg.valHidden, cfg.valLayers);
            }
            // else: warm-start from previous frame's weights

            trainTopology(enc, topo, data, norm, cfg);
            float psnr = trainValues(enc, val, data, norm, cfg);
            writeNVDB(outFile, data, enc, topo, val, psnr, cfg);
        }
    } else {
        printf("\n============================================\n");
        printf(" NeuralVDB Encoder — Single Frame\n");
        printf("============================================\n");
        printf("  Input:  %s\n", inputPath.c_str());

        auto data = extractVDB(inputPath, gridName);
        if (data.values.empty()) { std::cerr << "No data extracted.\n"; return 1; }

        auto norm = computeNorm(data);

        PosEncoder enc(cfg.numFreq);
        TopoNet topo(enc->dim(), cfg.topoHidden, cfg.topoLayers);
        ValueNet val(enc->dim(), cfg.valHidden, cfg.valLayers);

        printf("\n");
        trainTopology(enc, topo, data, norm, cfg);
        float psnr = trainValues(enc, val, data, norm, cfg);
        writeNVDB(outputPath, data, enc, topo, val, psnr, cfg);
    }

    printf("\nDone.\n");
    return 0;
}
