#pragma once

// OpenVDB MUST be included before Nuke's DDImage headers.
// Nuke's ChannelSet.h defines a macro: #define foreach(VAR, CHANNELS)
// which collides with OpenVDB's TypeList::foreach() member functions.
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

    // 3D display knobs
    bool        _showBbox      = true;
    bool        _showPoints    = true;
    int         _pointDensity  = 1;    // 0=Low 1=Med 2=High
    double      _pointSize     = 3.0;

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

    // ── Volume transform from Axis input ──
    bool   _hasVolumeXform = false;
    double _volFwd[4][4];     // volume local → world
    double _volInv[4][4];     // world → volume local

    // ── 3D viewport point cloud ──
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

    DD::Image::Lock _loadLock;
    std::string     _loadedPath;
    std::string     _loadedGrid;

    static bool _vdbInitialised;
};
