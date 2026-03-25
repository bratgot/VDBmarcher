# NeuralVDB Integration Guide for VDBRenderIop.cpp
# ================================================
#
# This file documents every change needed to add neural decode support
# to your existing VDBRenderIop.cpp. Each change is a find→replace block.
#
# The key principle: replace every BoxSampler::sample() call on density
# and shadow accessors with ctx.sampleDensity(iP) / ctx.sampleShadow(iP).
# The MarchCtx methods dispatch to either BoxSampler or NeuralDecoder
# depending on whether a .nvdb file is loaded.
#
# Temperature and flame grids stay on OpenVDB accessors — the neural
# decoder only replaces density (the primary grid). Neural temp/flame
# support can be added later by extending the .nvdb format.

# ═══════════════════════════════════════════════════════════════════
# CHANGE 1: Knobs — add Neural tab
# ═══════════════════════════════════════════════════════════════════
#
# After the Quality tab's closing brace (after line ~604), add:

#ifdef VDBRENDER_HAS_NEURAL
    Tab_knob(f,"Neural");

    Text_knob(f,
        "<font size='-1' color='#777'>"
        "NeuralVDB compresses VDB files 10-100x using neural networks.<br>"
        "Load a .nvdb file (created by nvdb_encoder.py) and all existing<br>"
        "render modes, lighting, and deep output work automatically."
        "</font>");

    Divider(f,"Neural Decode");

    Bool_knob(f,&_neuralUseCuda,"neural_cuda","CUDA Inference");
    Tooltip(f,"Use GPU for neural network inference.\n"
              "Requires CUDA-compatible GPU and LibTorch+CUDA build.\n"
              "Falls back to CPU if unavailable.");

    // Read-only info
    String_knob(f,&_neuralInfoRatio,"neural_info_ratio","Compression");
    SetFlags(f,Knob::READ_ONLY|Knob::DO_NOT_WRITE);

    String_knob(f,&_neuralInfoPSNR,"neural_info_psnr","PSNR");
    SetFlags(f,Knob::READ_ONLY|Knob::DO_NOT_WRITE);

    BeginClosedGroup(f,"grp_neural_info","Neural Technical Reference");
    Text_knob(f,
        "<font size='-1' color='#bbb'>"
        "<b>NeuralVDB</b> (Kim et al. 2024) replaces lower VDB tree<br>"
        "nodes with compact neural networks. The upper tree provides<br>"
        "HDDA empty-space skipping. Two MLPs per volume:<br>"
        "1. Topology classifier — predicts active voxel mask<br>"
        "2. Value regressor — predicts density at active positions<br>"
        "Both use Fourier positional encoding for detail."
        "</font><br><br>"
        "<font size='-1' color='#bbb'>"
        "<b>Sampling</b> — Neural decode replaces BoxSampler at the<br>"
        "leaf level. All lighting, shadows, phase function, multi-<br>"
        "scatter, deep output, and AOVs work identically."
        "</font><br><br>"
        "<font size='-1' color='#888'>"
        "Kim, Museth et al. (2024) — NeuralVDB, ACM TOG"
        "</font>");
    EndGroup(f);
#endif


# ═══════════════════════════════════════════════════════════════════
# CHANGE 2: File loading in _validate — detect .nvdb
# ═══════════════════════════════════════════════════════════════════
#
# In the VDB loading section of _validate (around line 1258-1322),
# add .nvdb detection BEFORE the OpenVDB file loading block.
# The key: if the file ends in .nvdb, load via NeuralDecoder instead
# of openvdb::io::File, then create a synthetic _floatGrid from the
# upper tree so all downstream code (bbox, VRI, etc.) still works.

# INSERT BEFORE: "if(!_gridValid||path2!=_loadedPath..." (line ~1265)

#ifdef VDBRENDER_HAS_NEURAL
    // Check if this is a .nvdb neural compressed file
    bool isNVDB = false;
    if (path2.size() > 5 && path2.substr(path2.size()-5) == ".nvdb")
        isNVDB = true;

    if (isNVDB) {
        // Neural decode path
        if (!_neural) _neural = std::make_unique<neural::NeuralDecoder>();
        if (!_neuralMode || path2 != _loadedPath || curFrame != _loadedFrame) {
            _neural->unload();
            _neuralMode = false;
            _floatGrid.reset(); _tempGrid.reset(); _flameGrid.reset();
            _gridValid = false; _previewPoints.clear();

            if (_neural->load(path2, _neuralUseCuda)) {
                // Use upper tree as the density grid — provides HDDA topology
                _floatGrid = _neural->upperGrid();
                _neuralMode = true;

                // Compute bbox from upper tree
                auto ab = _floatGrid->evalActiveVoxelBoundingBox();
                if (!ab.empty()) {
                    const auto& xf = _floatGrid->transform();
                    openvdb::Vec3d corners[8]; int ci2 = 0;
                    for (int iz=0;iz<=1;++iz) for (int iy=0;iy<=1;++iy) for (int ix=0;ix<=1;++ix)
                        corners[ci2++] = xf.indexToWorld(openvdb::Vec3d(
                            ix ? ab.max().x()+1. : ab.min().x(),
                            iy ? ab.max().y()+1. : ab.min().y(),
                            iz ? ab.max().z()+1. : ab.min().z()));
                    _bboxMin = _bboxMax = corners[0];
                    for (int i=1;i<8;++i) for (int a=0;a<3;++a) {
                        _bboxMin[a] = std::min(_bboxMin[a], corners[i][a]);
                        _bboxMax[a] = std::max(_bboxMax[a], corners[i][a]);
                    }
                    _gridValid = true;
                    _loadedPath = path2;
                    _loadedGrid = grid;
                    _loadedFrame = curFrame;

                    // Build VRI from upper tree — HDDA still works
                    _volRI.reset();
                    try { _volRI = std::make_unique<VRI>(*_floatGrid); } catch (...) {}

                    // Update info knobs
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.1fx", _neural->ratio());
                    _neuralInfoRatioStr = buf;
                    _neuralInfoRatio = _neuralInfoRatioStr.c_str();
                    std::snprintf(buf, sizeof(buf), "%.1f dB", _neural->psnr());
                    _neuralInfoPSNRStr = buf;
                    _neuralInfoPSNR = _neuralInfoPSNRStr.c_str();
                } else {
                    error("NeuralVDB: no active voxels in upper tree");
                }
            } else {
                error("Failed to load .nvdb file: %s", path2.c_str());
            }
        }
    } else {
        // Standard OpenVDB path — existing code follows unchanged
        _neuralMode = false;
#endif

# AND close the else block after the existing OpenVDB loading:
# INSERT AFTER the existing file.close() + _gridValid=true block:

#ifdef VDBRENDER_HAS_NEURAL
    } // close else from isNVDB check
#endif


# ═══════════════════════════════════════════════════════════════════
# CHANGE 3: makeMarchCtx — set neural decoder pointer
# ═══════════════════════════════════════════════════════════════════
#
# In makeMarchCtx() (line ~1582), add at the end before return:

#ifdef VDBRENDER_HAS_NEURAL
    if (_neuralMode && _neural && _neural->loaded()) {
        c.neuralDec = _neural.get();
        c.neuralMode = true;
    }
#endif


# ═══════════════════════════════════════════════════════════════════
# CHANGE 4: Replace BoxSampler::sample calls in march functions
# ═══════════════════════════════════════════════════════════════════
#
# These are mechanical find→replace operations. The ctx.sampleDensity()
# and ctx.sampleShadow() methods handle the dispatch automatically.
#
# ── marchRay (Lit mode) ──
#
# FIND (shadeSample lambda, density):
#   float density=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
# REPLACE WITH:
#   float density=ctx.sampleDensity(iP)*(float)_densityMix;
#
# FIND (shadow ray loop):
#   lT*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,li)*ext*_shadowDensity*shStep);
# REPLACE WITH:
#   lT*=std::exp(-(double)ctx.sampleShadow(li)*ext*_shadowDensity*shStep);
#
# FIND (HDDA adaptive step):
#   float ld=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
# REPLACE WITH:
#   float ld=ctx.sampleDensity(iP)*(float)_densityMix;
#
# FIND (env map shadow):
#   eT*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,ei)*ext*_shadowDensity*shStep);
# REPLACE WITH:
#   eT*=std::exp(-(double)ctx.sampleShadow(ei)*ext*_shadowDensity*shStep);
#
# FIND (multi-scatter bounce density):
#   float bDen=openvdb::tools::BoxSampler::sample(*dAcc,bI)*(float)_densityMix;
# REPLACE WITH:
#   float bDen=ctx.sampleDensity(bI)*(float)_densityMix;
#
# FIND (multi-scatter shadow):
#   lT2*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,li2)*ext*_shadowDensity*shStep*2);
# REPLACE WITH:
#   lT2*=std::exp(-(double)ctx.sampleShadow(li2)*ext*_shadowDensity*shStep*2);
#
# ── marchRayExplosion ──
#
# FIND (density sample):
#   float density=openvdb::tools::BoxSampler::sample(*dAcc,iP)*(float)_densityMix;
# REPLACE WITH:
#   float density=ctx.sampleDensity(iP)*(float)_densityMix;
#
# FIND (shadow):
#   lT*=std::exp(-(double)openvdb::tools::BoxSampler::sample(shAcc,li)*ext*_shadowDensity*shStep);
# REPLACE WITH:
#   lT*=std::exp(-(double)ctx.sampleShadow(li)*ext*_shadowDensity*shStep);
#
# FIND (HDDA adaptive, two sites):
#   lastDen=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
# REPLACE WITH:
#   lastDen=ctx.sampleDensity(iP)*(float)_densityMix;
#
# ── marchRayDensity (ramp modes) ──
#
# FIND:
#   float d=openvdb::tools::BoxSampler::sample(acc,iP)*(float)_densityMix;
# REPLACE WITH:
#   float d=ctx.sampleDensity(iP)*(float)_densityMix;
#
# Temperature and flame samples stay as BoxSampler — they use their
# own dedicated accessors (tempAcc, flameAcc) which are not neurally
# compressed in v1.
#
# ── doDeepEngine ──
#
# Same pattern for density and shadow samples inside the deep march loop.
# Note: doDeepEngine creates its own accessors (dAcc, tAcc, fAcc) rather
# than using MarchCtx. For neural mode, replace dAcc->getValue() and
# BoxSampler density calls with:
#
#ifdef VDBRENDER_HAS_NEURAL
#   if (_neuralMode && _neural) {
#       auto iP = xf.worldToIndex(wP);
#       float density = _neural->sampleDensity(iP) * (float)_densityMix;
#       // ... rest of shading unchanged
#   } else
#endif
#   { /* existing dAcc code */ }


# ═══════════════════════════════════════════════════════════════════
# CHANGE 5: HELP string update
# ═══════════════════════════════════════════════════════════════════
#
# Update the HELP string to mention .nvdb support:

const char* VDBRenderIop::HELP =
    "VDBRender — OpenVDB Volume Ray Marcher\n\n"
    "Single and multiple scatter volume renderer\n"
    "with deep output, environment lighting,\n"
    "and Henyey-Greenstein phase function.\n\n"
#ifdef VDBRENDER_HAS_NEURAL
    "Supports .nvdb neural compressed volumes\n"
    "(10-100x smaller) alongside standard .vdb files.\n\n"
#endif
    "Inputs:\n"
    "  bg  (0) — Background plate (sets resolution)\n"
    "  cam (1) — Camera\n"
    "  scn (2) — Scene (lights, axis, transforms)\n\n"
    "Created by Marten Blumen\n"
    "github.com/bratgot/VDBmarcher";


# ═══════════════════════════════════════════════════════════════════
# CHANGE 6: append() — include neural state in hash
# ═══════════════════════════════════════════════════════════════════
#
# In append(Hash& hash), add:

#ifdef VDBRENDER_HAS_NEURAL
    hash.append(_neuralMode);
    hash.append(_neuralUseCuda);
#endif


# ═══════════════════════════════════════════════════════════════════
# CHANGE 7: Version string
# ═══════════════════════════════════════════════════════════════════
#
# Update the version text in knobs():
#
# FIND:
#   " <font color='#888' size='-1'>v2.1</font><br>"
# REPLACE:
#   " <font color='#888' size='-1'>v3.0</font><br>"
#   "<font color='#aaa'>OpenVDB volume ray marcher"
#ifdef VDBRENDER_HAS_NEURAL
#   " + NeuralVDB"
#endif
#   "</font>"
