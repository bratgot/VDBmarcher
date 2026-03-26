#pragma once
// VDBRender — OpenVDB Volume Ray Marcher for Nuke 17
// Created by Marten Blumen
// [NEURAL] NeuralVDB integration — neural compressed volume support

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/RayIntersector.h>
#include <openvdb/math/Ray.h>

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
    double _flameIntensity    = 5.0;
    double _rampIntensity     = 1.0;
    double _gradStart[3]      = {0,0,0};
    double _gradEnd[3]        = {1,0.8,0.2};

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
    int    _multiBounces      = 0;
    int    _bounceRays        = 6;
    int    _scatterPreset     = 0;
    int    _qualityPreset     = 0;
    bool   _adaptiveStep      = true;
    int    _proxyMode         = 0;

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

    // ── Point cloud ──
    struct DensityPoint { float x,y,z,density; };
    std::vector<DensityPoint> _previewPoints;
    float _maxDensity=1.0f;
    int _cachedPointDensity=-1; std::string _cachedPointsPath;
    int _cachedPointsFrame=-1; bool _cachedHasXform=false;
    double _cachedVolFwd[4][4]={};
    void rebuildPointCloud();

    static constexpr int _bboxEdges[12][2]={
        {0,1},{2,3},{4,5},{6,7},{0,2},{1,3},{4,6},{5,7},{0,4},{1,5},{2,6},{3,7}};

    DD::Image::CameraOp* camera() const;
    std::string resolveFramePath(int frame) const;
    void discoverGrids();

    // ══════════════════════════════════════════════════════════
    //  [NEURAL] NeuralVDB integration
    // ══════════════════════════════════════════════════════════

#ifdef VDBRENDER_HAS_NEURAL
    // Neural decoder — loads .nvdb files, provides sampleDensity()
    std::unique_ptr<neural::NeuralDecoder> _neural;
    bool _neuralMode       = false;   // true when live neural (not decoded)
    bool _neuralUseCuda    = false;   // knob: use CUDA for inference
    bool _neuralDecodeToGrid = true;  // knob: decode to grid at load (recommended)
    int  _neuralBatchSize  = 4096;    // knob: voxels per forward pass
    float _neuralTopoThreshold = 0.5f;

    // Info strings (read-only knobs)
    const char* _neuralInfoRatio = "";
    const char* _neuralInfoPSNR  = "";
    const char* _neuralInfoMode  = "";
    std::string _neuralInfoRatioStr, _neuralInfoPSNRStr, _neuralInfoModeStr;
#endif

    // ══════════════════════════════════════════════════════════
    //  Per-scanline march context
    // ══════════════════════════════════════════════════════════

    // MarchCtx pools grid accessors per scanline for thread safety.
    // [NEURAL] Adds sampling abstraction that dispatches to OpenVDB or neural.
    struct MarchCtx {
        openvdb::FloatGrid::ConstAccessor densAcc;
        openvdb::FloatGrid::ConstAccessor shAcc;
        std::unique_ptr<openvdb::FloatGrid::ConstAccessor> tempAcc;
        std::unique_ptr<openvdb::FloatGrid::ConstAccessor> flameAcc;
        std::unique_ptr<openvdb::Vec3SGrid::ConstAccessor> velAcc;
        std::unique_ptr<openvdb::Vec3SGrid::ConstAccessor> colorAcc;
        double step, ext, scat, g, g2, hgN;
        int nSh; double bDiag, shStep;

#ifdef VDBRENDER_HAS_NEURAL
        // [NEURAL] Neural decode state — set by makeMarchCtx when _neuralMode
        const neural::NeuralDecoder* neuralDec = nullptr;
        bool neuralMode = false;

        // [NEURAL] Unified density sample: BoxSampler or neural decode
        // Call this everywhere instead of BoxSampler::sample(densAcc, iP)
        inline float sampleDensity(const openvdb::Vec3d& iP) const {
            if (neuralMode && neuralDec)
                return neuralDec->sampleDensity(iP);
            return openvdb::tools::BoxSampler::sample(densAcc, iP);
        }

        // [NEURAL] Unified shadow sample
        inline float sampleShadow(const openvdb::Vec3d& iP) const {
            if (neuralMode && neuralDec)
                return neuralDec->sampleDensity(iP); // shadow uses same density
            return openvdb::tools::BoxSampler::sample(shAcc, iP);
        }
#else
        // Non-neural build: direct BoxSampler (zero overhead, inlined)
        inline float sampleDensity(const openvdb::Vec3d& iP) const {
            return openvdb::tools::BoxSampler::sample(densAcc, iP);
        }
        inline float sampleShadow(const openvdb::Vec3d& iP) const {
            return openvdb::tools::BoxSampler::sample(shAcc, iP);
        }
#endif

        MarchCtx(const openvdb::FloatGrid::ConstAccessor&d,const openvdb::FloatGrid::ConstAccessor&s)
            :densAcc(d),shAcc(s),step(0),ext(0),scat(0),g(0),g2(0),hgN(0),nSh(1),bDiag(0),shStep(0){}
        MarchCtx(MarchCtx&&)=default;
        MarchCtx&operator=(MarchCtx&&)=default;
    };
    MarchCtx makeMarchCtx() const;

    void marchRay(MarchCtx&ctx,const openvdb::Vec3d&o,const openvdb::Vec3d&d,float&R,float&G,float&B,float&A,float&emR,float&emG,float&emB) const;
    void marchRayExplosion(MarchCtx&ctx,const openvdb::Vec3d&o,const openvdb::Vec3d&d,float&R,float&G,float&B,float&A,float&emR,float&emG,float&emB) const;
    void marchRayDensity(MarchCtx&ctx,const openvdb::Vec3d&o,const openvdb::Vec3d&d,float&den,float&alpha) const;

    DD::Image::Lock _loadLock;
    std::string _loadedPath, _loadedGrid;
    static bool _vdbInitialised;
};
