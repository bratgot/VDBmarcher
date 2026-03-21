#pragma once

// OpenVDB MUST be included before Nuke's DDImage headers.
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Interpolation.h>

// Now include Nuke headers
#include <DDImage/Iop.h>
#include <DDImage/Knobs.h>
#include <DDImage/Row.h>
#include <DDImage/Thread.h>
#include <DDImage/CameraOp.h>
#include <DDImage/AxisOp.h>
#include <DDImage/Matrix4.h>
#include <DDImage/Format.h>
#include <DDImage/ViewerContext.h>
#include <DDImage/gl.h>

#include <string>
#include <vector>
#include <memory>

class VDBRenderIop : public DD::Image::Iop
{
public:
    explicit VDBRenderIop(Node* node);
    ~VDBRenderIop() override = default;

    void        knobs(DD::Image::Knob_Callback f) override;
    int         knob_changed(DD::Image::Knob* k) override;
    const char* Class()     const override { return CLASS; }
    const char* node_help() const override { return HELP; }

    int         minimum_inputs() const override { return 1; }
    int         maximum_inputs() const override { return 2; }
    const char* input_label(int idx, char* buf) const override;
    bool        test_input(int idx, DD::Image::Op* op) const override;
    Op*         default_input(int idx) const override;

    void _validate(bool for_real) override;
    void _request(int x, int y, int r, int t,
                  DD::Image::ChannelMask, int count) override;
    void engine(int y, int x, int r,
                DD::Image::ChannelMask, DD::Image::Row&) override;
    void append(DD::Image::Hash& hash) override;

    void build_handles(DD::Image::ViewerContext* ctx) override;
    void draw_handle(DD::Image::ViewerContext* ctx) override;

    static const DD::Image::Op::Description desc;
    static const char* CLASS;
    static const char* HELP;

    // Color scheme enum — shared between 2D and 3D
    enum ColorScheme {
        kLit = 0,        // Standard ray-marched lighting (2D only, 3D falls back to greyscale)
        kGreyscale,
        kHeat,
        kCool,
        kBlackbody,
        kCustomGradient
    };

    // Evaluate a color ramp for a normalised density value [0,1]
    struct Color3 { float r, g, b; };
    static Color3 evalRamp(ColorScheme scheme, float t,
                           const float* gradA, const float* gradB,
                           double tempMin, double tempMax);
    static Color3 blackbody(double tempKelvin);

private:
    // ── Knobs ──
    const char* _vdbFilePath   = "";
    const char* _gridName      = "density";
    double      _stepSize      = 0.5;
    double      _extinction    = 5.0;
    double      _scattering    = 3.0;
    double      _lightDir[3]   = { 0.577, 0.577, 0.577 };
    double      _lightColor[3] = { 1.0,   1.0,   1.0   };
    int         _frameOffset   = 0;
    DD::Image::FormatPair _formats;

    // Color scheme
    int         _colorScheme   = kLit;
    double      _tempMin       = 500.0;
    double      _tempMax       = 6500.0;
    double      _gradStart[3]  = { 0.0, 0.0, 0.0 };
    double      _gradEnd[3]    = { 1.0, 0.8, 0.2 };

    // 3D display
    bool        _showBbox      = true;
    bool        _showPoints    = true;
    int         _pointDensity  = 1;
    double      _pointSize     = 3.0;
    int         _viewportColor = kHeat;  // separate scheme for 3D

    // ── Grid state ──
    openvdb::FloatGrid::Ptr _floatGrid;
    openvdb::Vec3d          _bboxMin;
    openvdb::Vec3d          _bboxMax;
    bool                    _gridValid = false;
    int                     _loadedFrame = -1;

    // ── Camera cache ──
    openvdb::Vec3d  _camOrigin;
    double          _camRot[3][3];
    double          _halfW    = 1.0;
    bool            _camValid = false;

    // ── Volume transform ──
    bool   _hasVolumeXform = false;
    double _volFwd[4][4];
    double _volInv[4][4];

    // ── 3D point cloud cache ──
    struct DensityPoint { float x, y, z, density; };
    std::vector<DensityPoint> _previewPoints;
    float                     _maxDensity = 1.0f;
    int                       _cachedPointDensity = -1;
    std::string               _cachedPointsPath;
    int                       _cachedPointsFrame = -1;
    bool                      _cachedHasXform = false;
    void rebuildPointCloud();

    static constexpr int _bboxEdges[12][2] = {
        {0,1},{2,3},{4,5},{6,7},
        {0,2},{1,3},{4,6},{5,7},
        {0,4},{1,5},{2,6},{3,7}
    };

    DD::Image::CameraOp* camera() const;
    DD::Image::AxisOp*   axisInput() const;
    std::string resolveFramePath(int frame) const;

    void marchRay(const openvdb::Vec3d& origin,
                  const openvdb::Vec3d& dir,
                  float& outR, float& outG,
                  float& outB, float& outA) const;

    // Unlit density pass for color-mapped modes
    void marchRayDensity(const openvdb::Vec3d& origin,
                         const openvdb::Vec3d& dir,
                         float& outDensity, float& outAlpha) const;

    DD::Image::Lock _loadLock;
    std::string     _loadedPath;
    std::string     _loadedGrid;

    static bool _vdbInitialised;
};
