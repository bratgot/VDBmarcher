#pragma once
// VDBRender — OpenVDB Volume Ray Marcher for Nuke 17
// Created by Marten Blumen

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>

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
    int         maximum_inputs() const override { return 11; }
    const char* input_label(int idx, char* buf) const override;
    bool        test_input(int idx, DD::Image::Op* op) const override;
    Op*         default_input(int idx) const override;

    // ── 2D Iop ──
    void _validate(bool for_real) override;
    void _request(int x, int y, int r, int t, DD::Image::ChannelMask, int count) override;
    void engine(int y, int x, int r, DD::Image::ChannelMask, DD::Image::Row&) override;
    void append(DD::Image::Hash& hash) override;
    void build_handles(DD::Image::ViewerContext* ctx) override;
    void draw_handle(DD::Image::ViewerContext* ctx) override;

    // ── DeepOp interface ──
    // DeepOp requires op() to return the Op* for this node
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
    const char* _gridName      = "density";
    const char* _tempGridName  = "";
    const char* _flameGridName = "";
    double _densityMix        = 1.0;
    double _tempMix           = 1.0;
    double _flameMix          = 1.0;
    int    _frameOffset       = 0;
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
    double _ambientIntensity  = 0.0;

    // ── Quality ──
    double _quality           = 2.0;
    int    _shadowSteps       = 8;
    double _shadowDensity     = 1.0;
    int    _deepSamples       = 32;
    int    _multiBounces      = 0;    // 0=single scatter, 1-4=multi-scatter bounces
    int    _bounceRays        = 6;    // directions per bounce

    // ── Viewport ──
    bool   _showBbox          = true;
    bool   _showPoints        = true;
    int    _pointDensity      = 1;
    double _pointSize         = 3.0;
    bool   _linkViewport      = true;
    int    _viewportColor     = 1;

    // ── Grid state ──
    openvdb::FloatGrid::Ptr _floatGrid, _tempGrid, _flameGrid;
    openvdb::Vec3d _bboxMin, _bboxMax;
    bool _gridValid=false, _hasTempGrid=false, _hasFlameGrid=false;
    int  _loadedFrame=-1;

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
    DD::Image::AxisOp* axisInput() const;
    std::string resolveFramePath(int frame) const;
    void discoverGrids();

    void marchRay(const openvdb::Vec3d&o,const openvdb::Vec3d&d,float&R,float&G,float&B,float&A) const;
    void marchRayExplosion(const openvdb::Vec3d&o,const openvdb::Vec3d&d,float&R,float&G,float&B,float&A) const;
    void marchRayDensity(const openvdb::Vec3d&o,const openvdb::Vec3d&d,float&den,float&alpha) const;

    DD::Image::Lock _loadLock;
    std::string _loadedPath, _loadedGrid;
    static bool _vdbInitialised;
};
