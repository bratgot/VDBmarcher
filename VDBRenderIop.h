#pragma once
// VDBRender — OpenVDB Volume Ray Marcher for Nuke 17
// Created by Marten Blumen
// [NEURAL] NeuralVDB integration — neural compressed volume support
// [V2] Dual-lobe HG · powder effect · jitter · gradient normals
//      analytical multi-scatter (Wrenninge 2015) · chromatic extinction

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/RayIntersector.h>
#include <openvdb/math/Ray.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointCount.h>

#ifdef VDBRENDER_HAS_NEURAL
#include "NeuralDecoder.h"
#endif

#include <DDImage/Iop.h>
#include <DDImage/DeepOp.h>
#include <DDImage/DeepPlane.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>
#include <DDImage/Thread.h>
#include <DDImage/CameraOp.h>
#include <DDImage/AxisOp.h>
#include <DDImage/LightOp.h>
#include <DDImage/Matrix4.h>
#include <DDImage/Format.h>
#include <DDImage/ViewerContext.h>
#include <DDImage/gl.h>

#include <string>
#include <vector>
#include <memory>
#include <cstring>

class VDBRenderIop : public DD::Image::Iop, public DD::Image::DeepOp
{
public:
    explicit VDBRenderIop(Node* node);
    ~VDBRenderIop() override = default;

    void        knobs(DD::Image::Knob_Callback f) override;
    int         knob_changed(DD::Image::Knob* k) override;
    const char* Class()     const override { return CLASS; }
    const char* node_help() const override { return HELP; }

    int         minimum_inputs() const override { return 3; }
    int         maximum_inputs() const override { return 3; }
    const char* input_label(int idx, char* buf) const override;
    bool        test_input(int idx, DD::Image::Op* op) const override;
    Op*         default_input(int idx) const override;

    void _validate(bool for_real) override;
    void _open() override;

    void _request(int x, int y, int r, int t, DD::Image::ChannelMask, int count) override;
    void engine(int y, int x, int r, DD::Image::ChannelMask, DD::Image::Row&) override;
    void append(DD::Image::Hash& hash) override;
    void build_handles(DD::Image::ViewerContext* ctx) override;
    void draw_handle(DD::Image::ViewerContext* ctx) override;

    DD::Image::Op* op() override { return static_cast<DD::Image::Iop*>(this); }
    void getDeepRequests(DD::Image::Box box, const DD::Image::ChannelSet& channels,
                         int count, std::vector<DD::Image::RequestData>& reqData) override;
    bool doDeepEngine(DD::Image::Box box, const DD::Image::ChannelSet& channels,
                      DD::Image::DeepOutputPlane& plane) override;

    static const DD::Image::Op::Description desc;
    static const char* CLASS;
    static const char* HELP;

    enum ColorScheme { kLit=0, kGreyscale, kHeat, kCool, kBlackbody, kCustomGradient, kExplosion };
    struct Color3 { float r, g, b; };
    static Color3 evalRamp(ColorScheme s, float t, const float* gA, const float* gB, double tMin, double tMax);
    static Color3 blackbody(double kelvin);

private:
    // ── File ──
    const char* _vdbFilePath   = "";
    const char* _vdbFilePath2  = "";   // second VDB layer
    const char* _gridName2     = "";   // density grid name for layer 2
    double      _densityMix2   = 1.0;  // density mix for layer 2
    bool        _grid2Enable   = false; // enable/disable layer 2

    // ── Point cloud rendering ──
    const char* _pointGridName  = "";    // PointDataGrid name in VDB file
    double      _pointRadius    = 0.05;  // world-space default radius
    double      _pointIntensity = 1.0;   // brightness multiplier
    bool        _pointLit       = true;  // apply lights to particles
    bool   _autoSequence       = false;
    const char* _origFilePath  = "";
    const char* _gridName      = "density";
    const char* _tempGridName  = "";
    const char* _flameGridName = "";
    double _densityMix = 1.0, _tempMix = 1.0, _flameMix = 1.0;
    int    _frameOffset = 0;
    DD::Image::FormatPair _formats;

    // ── Shading ──
    int    _scenePreset       = 0;
    int    _colorScheme       = kLit;
    double _extinction        = 5.0;
    double _scattering        = 3.0;
    double _anisotropy        = 0.0;
    int    _anisotropyPreset  = 0;
    double _tempMin           = 500.0;
    double _tempMax           = 6500.0;
    double _emissionIntensity = 2.0;
    bool   _emissionRampEnable = false;  // use custom ramp instead of blackbody
    double _emissionRampLow[3]  = {0.0, 0.0, 0.0};  // colour at temp=0
    double _emissionRampHigh[3] = {1.0, 1.0, 1.0};  // colour at temp=1
    double _flameIntensity    = 5.0;
    double _rampIntensity     = 1.0;
    double _gradStart[3]      = {0,0,0};
    double _gradEnd[3]        = {1,0.8,0.2};

    // ── [V2] Phase function — dual-lobe Henyey-Greenstein ──
    // Replaces single-lobe 'anisotropy' for Lit and Explosion modes.
    // g_forward: forward-scatter lobe (0 = isotropic, 1 = full forward)
    // g_backward: backward-scatter lobe (0 = isotropic, -1 = full backward)
    // lobe_mix: weight of forward lobe (1.0 = pure forward, 0.0 = pure backward)
    double _gForward      = 0.65;
    double _gBackward     = -0.25;
    double _lobeMix       = 0.70;

    // ── [V2] Scatter quality ──
    // powder_strength: interior brightening (Schneider & Vos 2015, HZD clouds)
    //   0 = off, 2 = natural, 4-6 = dense explosion fireball, 10 = maximum
    // gradient_mix: blend density-gradient Lambertian term with HG phase
    //   0 = off (HG only), 0.3 = clouds, 0 = smoke/fire (perf cost: 6 extra lookups/step)
    // jitter: stochastic step offset — eliminates wood-grain banding
    double _powderStrength = 2.0;
    double _gradientMix    = 0.0;
    bool   _jitter         = true;

    // ── [V4] Procedural detail noise ──
    // fBm Perlin noise perturbs density at render time — adds micro-detail
    // beyond VDB resolution without requiring a higher-res grid.
    // noise_scale: world-space frequency multiplier (1 = one cycle per bbox)
    // noise_strength: how much noise modulates density (0 = off, 1 = full)
    // noise_octaves: fBm octave count (1 = smooth, 4 = highly detailed)
    // noise_roughness: lacunarity per octave (0 = smooth, 0.5 = natural, 1 = rough)
    bool   _noiseEnable    = false;
    double _noiseScale     = 4.0;
    double _noiseStrength  = 0.5;
    int    _noiseOctaves   = 3;
    double _noiseRoughness = 0.5;

    // ── [V4] Phase function mode ──
    // 0 = Dual-lobe HG (default, fast)
    // 1 = Approximate Mie (Jendersie & d'Eon, SIGGRAPH 2023)
    //     Physically accurate for water droplets (clouds, fog)
    //     Uses droplet diameter d (micrometers) to fit Mie lobe shape
    int    _phaseMode      = 0;
    double _mieDropletD    = 2.0;    // droplet diameter in µm (0.1=aerosol, 2=cloud, 10=rain)

    // ── [V2] Analytical multiple scattering (Wrenninge 2015) ──
    // Replaces brute-force N-bounce with a 2-param fit to the diffusion solution.
    // Zero extra rays — same cost as single scatter.
    // ms_tint: color of the multiple-scatter contribution (slightly warm/cool bias)
    bool   _msApprox    = true;
    double _msTint[3]   = {1.0, 0.97, 0.95};

    // ── [V2] Chromatic extinction ──
    // Per-channel σt (extinction coefficient). Real smoke/dust scatters blue
    // wavelengths more (Rayleigh-like): set ext_r < ext_g < ext_b for realism.
    // Shadow rays always use base _extinction (greyscale) for performance.
    bool   _chromaticExt = false;
    double _extR         = 5.0;   // mirrors _extinction on load
    double _extG         = 5.0;
    double _extB         = 5.0;

    // ── Lighting ──
    double _lightDir[3]       = {0.577, 0.577, -0.577};
    double _lightColor[3]     = {1,1,1};
    double _lightIntensity    = 1.0;
    bool   _useFallbackLight  = true;
    double _ambientIntensity  = 0.0;

    // ── Light Rig ──
    int    _skyPreset         = 2;
    double _skyMix            = 1.0;
    double _sunElevation      = 45.0;
    double _sunAzimuth        = 180.0;
    double _sunIntensity      = 3.0;
    double _skyIntensity      = 0.4;
    double _turbidity         = 3.0;
    double _groundBounce      = 0.1;
    int    _studioPreset      = 0;
    double _studioMix         = 0.0;
    double _studioKeyAzimuth  = 45.0;
    double _studioKeyElevation= 40.0;
    double _studioKeyIntensity= 3.0;
    double _studioFillRatio   = 0.35;
    double _studioRimIntensity= 2.0;
    void   buildLightRig();
    double _envIntensity      = 1.0;
    double _envDiffuse        = 0.5;
    double _envRotate         = 0.0;

    // ── Quality ──
    double _quality           = 3.0;
    int    _shadowSteps       = 8;
    double _shadowDensity     = 1.0;
    int    _deepSamples       = 32;
    int    _renderSamples     = 1;    // stochastic passes per pixel, averaged
    int    _multiBounces      = 0;   // kept for backward compat; msApprox supersedes
    int    _bounceRays        = 6;
    int    _scatterPreset     = 0;
    int    _qualityPreset     = 0;
    bool   _adaptiveStep      = true;
    int    _proxyMode         = 0;
    bool   _renderRegionEnable = false;
    double _rrX = 0.0, _rrY = 0.0, _rrW = 1.0, _rrH = 1.0; // normalised 0-1

    // ── Motion blur ──
    const char* _velGridName  = "";
    bool   _motionBlur        = false;
    int    _shutterPreset     = 1;
    double _shutterOpen       = -0.5;
    double _shutterClose      = 0.5;
    int    _motionSamples     = 3;

    // ── AOV / extra outputs ──
    bool   _aovDensity        = false;
    bool   _aovEmission       = false;
    bool   _aovShadow         = false;
    bool   _aovDepth          = false;
    bool   _aovLights         = false;
    bool   _aovMotion         = false;   // screen-space motion vector AOV
    bool   _aovAlbedo         = false;   // unlit scatter albedo (for denoiser)
    bool   _aovNormal         = false;   // density gradient normal (for denoiser)
    static constexpr int kMaxLightAOVs = 4;

    // ── Vec3 colour grid ──
    const char* _colorGridName = "";
    openvdb::Vec3SGrid::Ptr _colorGrid;
    bool _hasColorGrid        = false;

    // ── Viewport ──
    bool   _showBbox          = true;
    bool   _showPoints        = true;
    int    _pointDensity      = 1;
    double _pointSize         = 3.0;
    bool   _linkViewport      = true;
    int    _viewportColor     = 1;

    // ── Grid state ──
    openvdb::FloatGrid::Ptr _floatGrid, _tempGrid, _flameGrid;
    openvdb::FloatGrid::Ptr _floatGrid2;
    openvdb::points::PointDataGrid::Ptr _pointGrid;  // particle point cloud grid
    bool _hasPointGrid = false;
    bool _grid2Valid = false;
    openvdb::Vec3SGrid::Ptr _velGrid;
    openvdb::Vec3d _bboxMin, _bboxMax;
    bool _gridValid=false, _hasTempGrid=false, _hasFlameGrid=false;
    bool _hasVelGrid=false;
    int  _loadedFrame=-1;

    // ── Ray acceleration (HDDA empty-space skipping) ──
    using VRI = openvdb::tools::VolumeRayIntersector<openvdb::FloatGrid>;
    std::unique_ptr<VRI> _volRI;

    // ── Camera ──
    openvdb::Vec3d _camOrigin;
    double _camRot[3][3], _halfW=1.0;
    bool _camValid=false;

    // ── Volume transform ──
    bool _hasVolumeXform=false;
    double _volFwd[4][4], _volInv[4][4];

    // ── Lights ──
    struct CachedLight { double dir[3], color[3], pos[3]; bool isPoint; };
    std::vector<CachedLight> _lights;
    void gatherLights(DD::Image::Op* scnOp);

    // ── Environment map ──
    static constexpr int kEnvRes = 128;
    float  _envMap[kEnvRes][kEnvRes/2][3] = {};
    bool   _hasEnvMap = false;
    bool   _envDirty  = false;
    DD::Image::Iop* _envIop = nullptr;
    double _envLightRotY = 0;
    std::string _envFilePath;
    void   cacheEnvMap(DD::Image::Iop* envIop);
    void   sampleEnv(const openvdb::Vec3d& dir, float& r, float& g, float& b) const;

    // ── [V5] SH env lighting ──
    // 0 = Uniform dirs (original, accurate, slow)
    // 1 = SH + virtual lights (fast — replaces dir loop with 9 MADs)
    int    _envMode           = 1;          // knob: default SH
    int    _envVirtualLights  = 2;          // knob: how many peak dirs to extract as virtual dir lights
    // 9 L2 SH coefficients × 3 channels (projected in cacheEnvMap)
    double _envSH[9][3]       = {};
    bool   _hasEnvSH          = false;
    // Virtual directional lights extracted from env map peaks
    // These are appended to _lights and benefit from the V3 transmittance cache.
    int    _envVirtualLightBase = 0;        // index into _lights where virtual lights start

    // ── Point cloud ──
    struct DensityPoint { float x,y,z,density; float r,g,b; };  // r/g/b = pre-lit colour
    std::vector<DensityPoint> _previewPoints;
    float _maxDensity=1.0f;
    bool  _viewportLit=true;   // knob: use render lighting in viewport
    int _cachedPointDensity=-1; std::string _cachedPointsPath;
    int _cachedPointsFrame=-1; bool _cachedHasXform=false;
    double _cachedVolFwd[4][4]={};
    void rebuildPointCloud();

    static constexpr int _bboxEdges[12][2]={
        {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};

    DD::Image::CameraOp* camera() const;
    std::string resolveFramePath(int frame) const;
    void discoverGrids();

    // ── [V2] Phase function helpers ──
    static double hgRaw(double cosT, double g) noexcept;
    static double jitterHash(int x, int y) noexcept;

    // ── [V4] Procedural noise helpers ──
    static double noiseHash3(double x, double y, double z) noexcept;
    static double noiseFBm(double x, double y, double z, int octaves, double roughness) noexcept;

    // ── [V4] Mie phase function ──
    static double miePhaseS(double cosT, double d) noexcept;

    // ── [V5] SH environment lighting ──
    // Evaluates L2 spherical harmonic irradiance at direction (dx,dy,dz).
    // sh9 = array of 9 SH coefficients for one colour channel.
    // Returns irradiance clamped to [0, ∞).
    static double evalEnvSH(const double sh9[9],
                             double dx, double dy, double dz) noexcept;

    // ── [V6] ReSTIR env lighting ──────────────────────────────────────
    // Weighted reservoir sampling for env directions. Candidates are
    // weighted by SH radiance × phase. Only the selected direction
    // is shadow-traced — one ray per step regardless of nEnv.
    // Optional spatial reuse combines reservoirs from nearby pixels,
    // reducing variance without extra shadow rays.
    bool _useReSTIR = false;  // knob: enable ReSTIR env sampling

    struct Reservoir {
        int   dirIdx = -1;    // selected direction (index into kDirs26)
        float weight = 0.0f;  // target weight of selected sample
        float wSum   = 0.0f;  // sum of all candidate weights
        int   M      = 0;     // candidate count
        void update(int idx, float w, float rand01) {
            wSum += w;  ++M;
            if (rand01 * wSum < w) { dirIdx = idx; weight = w; }
        }
        // Unbiased contribution weight
        float W() const {
            return (M > 0 && weight > 1e-8f) ? wSum / (weight * M) : 0.0f;
        }
        void merge(const Reservoir& r, float rand01) {
            // Combine another reservoir into this one (spatial reuse)
            const float contrib = r.W() * r.weight * r.M;
            wSum += contrib;
            M    += r.M;
            if (r.dirIdx >= 0 && rand01 * wSum < contrib)
                { dirIdx = r.dirIdx; weight = r.weight; }
        }
    };

    // ── [V3] Shadow performance ──
    // Two-tier shadow system:
    //   Tier 1 — HDDA shadow rays: always active, skips empty VDB tiles.
    //   Tier 2 — Transmittance cache: precomputed FloatGrid per directional
    //             light, reduces shadow cost to O(1) per primary sample.
    bool _useShadowCache   = false;  // knob: enable tier-2 cache
    int  _shadowCacheRes   = 1;      // knob: 0=full, 1=half, 2=quarter res
    bool _shadowCacheDirty = true;   // invalidated when lights or grid change

    struct ShadowCache {
        openvdb::FloatGrid::Ptr transGrid;  // T(voxel) = transmittance toward light
        openvdb::Vec3d          lightDir;   // normalized light direction
        bool valid = false;
    };
    std::vector<ShadowCache> _shadowCaches;
    void buildShadowCaches();   // called from _open() on main thread
    int  _envDirCacheBase = -1; // index into _shadowCaches where env-dir caches start

    // ══════════════════════════════════════════════════════════
    //  [NEURAL] NeuralVDB integration
    // ══════════════════════════════════════════════════════════

#ifdef VDBRENDER_HAS_NEURAL
    std::unique_ptr<neural::NeuralDecoder> _neural;
    bool _neuralMode         = false;
    bool _neuralUseCuda      = false;
    bool _neuralDecodeToGrid = true;
    int  _neuralBatchSize    = 4096;
    float _neuralTopoThreshold = 0.5f;

    const char* _neuralInfoRatio = "";
    const char* _neuralInfoPSNR  = "";
    const char* _neuralInfoMode  = "";
    std::string _neuralInfoRatioStr, _neuralInfoPSNRStr, _neuralInfoModeStr;
#endif

    // ══════════════════════════════════════════════════════════
    //  Per-scanline march context
    // ══════════════════════════════════════════════════════════

    struct MarchCtx {
        // ── Grid accessors (thread-safe shallow copies) ──
        openvdb::FloatGrid::ConstAccessor densAcc;
        openvdb::FloatGrid::ConstAccessor shAcc;
        std::unique_ptr<openvdb::FloatGrid::ConstAccessor>   tempAcc;
        std::unique_ptr<openvdb::FloatGrid::ConstAccessor>   flameAcc;
        std::unique_ptr<openvdb::Vec3SGrid::ConstAccessor>   velAcc;
        std::unique_ptr<openvdb::Vec3SGrid::ConstAccessor>   colorAcc;

        // ── Original march params ──
        double step, ext, scat, g, g2, hgN;
        int    nSh;
        double bDiag, shStep;

        // ── [V2] Dual-lobe phase function ──
        double gFwd   = 0.65;
        double gBck   = -0.25;
        double lobeMix = 0.70;

        // ── [V2] Scatter quality ──
        double powder  = 2.0;   // powder effect strength (0 = off)
        double gradMix = 0.0;   // gradient-normal Lambertian blend (0 = off)

        // ── [V4] Procedural detail noise ──
        bool   noiseEnable    = false;
        double noiseScale     = 4.0;
        double noiseStrength  = 0.5;
        int    noiseOctaves   = 3;
        double noiseRoughness = 0.5;

        // ── [V4] Phase function mode ──
        // 0 = dual-lobe HG,  1 = approximate Mie (Jendersie 2023)
        int    phaseMode   = 0;
        double mieDropletD = 2.0;

        // ── [V2] Analytical multiple scattering ──
        bool   msApprox  = true;
        double msTintR   = 1.0;
        double msTintG   = 0.97;
        double msTintB   = 0.95;

        // ── [V2] Chromatic extinction ──
        bool   chromatic = false;
        double extR = 5.0;   // per-channel σt (primary ray only; shadow stays greyscale)
        double extG = 5.0;
        double extB = 5.0;

        // ── [V2] Jitter ──
        bool   jitter    = true;
        double jitterOff = 0.0;

        // ── [V5] SH environment lighting ──
        int    envMode   = 1;
        double envSH[9][3] = {};
        bool   hasEnvSH  = false;

        // ── [V6] ReSTIR ──
        bool   useReSTIR = false;

        // ── [V3] Shadow performance ──
        // shadowRI: per-scanline shallow copy of _volRI for HDDA shadow rays.
        // Always used when available — skips empty VDB tiles on shadow rays.
        std::unique_ptr<VRI> shadowRI;

        // Shadow transmittance cache accessors — one per light.
        // nullptr for point lights or when cache is disabled.
        std::vector<std::unique_ptr<openvdb::FloatGrid::ConstAccessor>> shadowCacheAcc;
        bool useShadowCache = false;

        // ── [V6] Env dir shadow cache ──
        // Indices into shadowCacheAcc for the 6 axis dirs used by SH env path.
        // -1 = no cache entry for that dir (falls back to HDDA).
        int envDirCacheIdx[6] = {-1,-1,-1,-1,-1,-1};

        // ── [V3.1] Per-light AOV accumulators ──
        // One RGB accumulator per light, up to kMaxLightAOVs.
        // Filled in shadeSample, returned through marchRay output params.
        bool  aovLights = false;
        float lightAovR[4] = {0,0,0,0};
        float lightAovG[4] = {0,0,0,0};
        float lightAovB[4] = {0,0,0,0};

#ifdef VDBRENDER_HAS_NEURAL
        const neural::NeuralDecoder* neuralDec = nullptr;
        bool neuralMode = false;

        inline float sampleDensity(const openvdb::Vec3d& iP) const {
            if (neuralMode && neuralDec) return neuralDec->sampleDensity(iP);
            return openvdb::tools::BoxSampler::sample(densAcc, iP);
        }
        inline float sampleShadow(const openvdb::Vec3d& iP) const {
            if (neuralMode && neuralDec) return neuralDec->sampleDensity(iP);
            return openvdb::tools::BoxSampler::sample(shAcc, iP);
        }
#else
        inline float sampleDensity(const openvdb::Vec3d& iP) const {
            return openvdb::tools::BoxSampler::sample(densAcc, iP);
        }
        inline float sampleShadow(const openvdb::Vec3d& iP) const {
            return openvdb::tools::BoxSampler::sample(shAcc, iP);
        }
#endif

        MarchCtx(const openvdb::FloatGrid::ConstAccessor& d,
                 const openvdb::FloatGrid::ConstAccessor& s)
            : densAcc(d), shAcc(s),
              step(0), ext(0), scat(0), g(0), g2(0), hgN(0),
              nSh(1), bDiag(0), shStep(0) {}

        MarchCtx(MarchCtx&&)            = default;
        MarchCtx& operator=(MarchCtx&&) = default;
    };

    MarchCtx makeMarchCtx() const;

    // marchRay: unified lit + explosion march.
    // explosionMode=true: emission evaluated before density check (fire-first ordering),
    // works without a density grid (fire-only volumes), uses emission as embedded light.
    void marchRay(MarchCtx& ctx, const openvdb::Vec3d& o, const openvdb::Vec3d& d,
                  float& R, float& G, float& B, float& A,
                  float& emR, float& emG, float& emB,
                  float& shR, float& shG, float& shB,
                  bool explosionMode = false) const;

    // marchRayExplosion: thin wrapper — calls marchRay(explosionMode=true).
    void marchRayExplosion(MarchCtx& ctx, const openvdb::Vec3d& o, const openvdb::Vec3d& d,
                           float& R, float& G, float& B, float& A,
                           float& emR, float& emG, float& emB,
                           float& shR, float& shG, float& shB) const;

    void marchRayDensity(MarchCtx& ctx, const openvdb::Vec3d& o, const openvdb::Vec3d& d,
                         float& den, float& alpha) const;

    // ── Point cloud rendering ──
    void renderPoints(int y, int x, int r,
                      const MarchCtx& ctx,
                      float* outR, float* outG, float* outB, float* outA) const;

    // [V3] Three-path shadow transmittance: cache lookup / HDDA / uniform march
    static double evalShadowTransmittance(
        MarchCtx& ctx, const openvdb::math::Transform& xf,
        const openvdb::Vec3d& wP, const openvdb::Vec3d& lD,
        double ext, double shDen, int lightIdx,
        const openvdb::Vec3d& bboxMin, const openvdb::Vec3d& bboxMax,
        int nSh, double shStep);

    DD::Image::Lock _loadLock;
    std::string _loadedPath, _loadedGrid;
    static bool _vdbInitialised;
};
