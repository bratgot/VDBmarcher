#include "VDBRenderIop.h"
#include <DDImage/Knob.h>
#include <DDImage/Format.h>
#include <cmath>
#include <algorithm>
#include <cstdio>

using namespace DD::Image;

const char* VDBRenderIop::CLASS = "VDBRender";
const char* VDBRenderIop::HELP  =
    "Ray-march an OpenVDB density grid into RGBA.\n\n"
    "Input 0: Camera (required)\n"
    "Input 1: Axis/Transform (optional — moves the volume)";

bool VDBRenderIop::_vdbInitialised = false;

// ═══════════════════════════════════════════════════════════════════════════════
// Color ramp evaluation
// ═══════════════════════════════════════════════════════════════════════════════

VDBRenderIop::Color3 VDBRenderIop::blackbody(double T)
{
    // Attempt: attempt to use a standard blackbody approximation
    // Based on CIE 1931 chromaticity + Planckian locus approximation
    // Valid roughly 1000K–40000K
    if (T < 100.0) T = 100.0;

    // Use approximation from Tanner Helland / CIE data
    double r, g, b;
    double t = T / 100.0;

    // Red
    if (t <= 66.0)
        r = 255.0;
    else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        r = std::clamp(r, 0.0, 255.0);
    }

    // Green
    if (t <= 66.0) {
        g = 99.4708025861 * std::log(t) - 161.1195681661;
        g = std::clamp(g, 0.0, 255.0);
    } else {
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
        g = std::clamp(g, 0.0, 255.0);
    }

    // Blue
    if (t >= 66.0)
        b = 255.0;
    else if (t <= 19.0)
        b = 0.0;
    else {
        b = 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
        b = std::clamp(b, 0.0, 255.0);
    }

    // Also modulate brightness by Stefan-Boltzmann (T^4) with normalisation
    // so that low temps are dim and high temps are bright
    double brightness = std::pow(T / 6500.0, 0.4); // perceptual scaling
    brightness = std::clamp(brightness, 0.0, 2.0);

    return {
        static_cast<float>(r / 255.0 * brightness),
        static_cast<float>(g / 255.0 * brightness),
        static_cast<float>(b / 255.0 * brightness)
    };
}

VDBRenderIop::Color3 VDBRenderIop::evalRamp(ColorScheme scheme, float t,
                                             const float* gradA, const float* gradB,
                                             double tempMin, double tempMax)
{
    t = std::clamp(t, 0.0f, 1.0f);

    switch (scheme) {
    default:
    case kGreyscale:
        return {t, t, t};

    case kHeat: {
        // Black → Red → Yellow → White
        if (t < 0.33f) {
            float s = t / 0.33f;
            return {s, 0.0f, 0.0f};
        } else if (t < 0.66f) {
            float s = (t - 0.33f) / 0.33f;
            return {1.0f, s, 0.0f};
        } else {
            float s = (t - 0.66f) / 0.34f;
            return {1.0f, 1.0f, s};
        }
    }

    case kCool: {
        // Black → Blue → Cyan → White
        if (t < 0.33f) {
            float s = t / 0.33f;
            return {0.0f, 0.0f, s};
        } else if (t < 0.66f) {
            float s = (t - 0.33f) / 0.33f;
            return {0.0f, s, 1.0f};
        } else {
            float s = (t - 0.66f) / 0.34f;
            return {s, 1.0f, 1.0f};
        }
    }

    case kBlackbody: {
        double temp = tempMin + t * (tempMax - tempMin);
        return blackbody(temp);
    }

    case kCustomGradient: {
        float r = gradA[0] + t * (gradB[0] - gradA[0]);
        float g = gradA[1] + t * (gradB[1] - gradA[1]);
        float b = gradA[2] + t * (gradB[2] - gradA[2]);
        return {r, g, b};
    }

    case kLit:
        // For 3D viewport, fall back to greyscale
        return {t, t, t};
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════════════════

VDBRenderIop::VDBRenderIop(Node* node) : Iop(node)
{
    inputs(2);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            _camRot[c][r] = (c == r) ? 1.0 : 0.0;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            _volFwd[i][j] = _volInv[i][j] = (i == j) ? 1.0 : 0.0;

    if (!_vdbInitialised) {
        openvdb::initialize();
        _vdbInitialised = true;
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// Knobs
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::knobs(Knob_Callback f)
{
    Text_knob(f, "<b>VDBRender</b> — OpenVDB Ray Marcher\n"
                 "Input 0: Camera  |  Input 1: Axis (optional)");
    Format_knob(f, &_formats, "format", "Output Format");

    Divider(f, "File");
    File_knob(f, &_vdbFilePath, "file", "VDB File");
    Tooltip(f, "Path to .vdb file. Sequences auto-detected.\n"
               "  dust_impact_0099.vdb → frame number auto-replaced\n"
               "  smoke.####.vdb or smoke.%04d.vdb also supported");
    String_knob(f, &_gridName, "grid_name", "Grid Name");
    Tooltip(f, "Grid name (default: 'density').");
    Int_knob(f, &_frameOffset, "frame_offset", "Frame Offset");

    Divider(f, "Ray March");
    Double_knob(f, &_stepSize,   "step_size",  "Step Size");
    Tooltip(f, "World-space step. 0.5 for preview, 0.05 for final.");
    Double_knob(f, &_extinction, "extinction", "Extinction");
    Tooltip(f, "Light absorption. Higher = denser.");
    Double_knob(f, &_scattering, "scattering", "Scattering");
    Tooltip(f, "Light scatter. Higher = brighter. (Only used in 'Lit' mode)");

    Divider(f, "Lighting (Lit mode only)");
    XYZ_knob(f,   _lightDir,   "light_dir",   "Light Direction");
    Color_knob(f, _lightColor, "light_color", "Light Color");

    Divider(f, "Render Color");
    static const char* schemeNames[] = {
        "Lit", "Greyscale", "Heat", "Cool", "Blackbody", "Custom Gradient", nullptr
    };
    Enumeration_knob(f, &_colorScheme, schemeNames, "color_scheme", "Render Mode");
    Tooltip(f, "Lit — standard ray-marched single-scatter lighting\n"
               "Greyscale — density mapped to white\n"
               "Heat — black/red/yellow/white ramp\n"
               "Cool — black/blue/cyan/white ramp\n"
               "Blackbody — physically-based temperature color\n"
               "Custom Gradient — user-defined two-color ramp");
    Double_knob(f, &_tempMin, "temp_min", "Temp Min (K)");
    Tooltip(f, "Minimum temperature in Kelvin for blackbody mapping.\n"
               "Density 0 maps to this temperature.");
    Double_knob(f, &_tempMax, "temp_max", "Temp Max (K)");
    Tooltip(f, "Maximum temperature. Density 1 (peak) maps here.\n"
               "Try 500–6500K for fire, 1000–15000K for explosions.");
    Color_knob(f, _gradStart, "grad_start", "Gradient Start");
    Tooltip(f, "Color at zero density (Custom Gradient mode).");
    Color_knob(f, _gradEnd,   "grad_end",   "Gradient End");
    Tooltip(f, "Color at peak density (Custom Gradient mode).");

    Divider(f, "3D Viewport");
    Bool_knob(f, &_showBbox, "show_bbox", "Show Bounding Box");
    Bool_knob(f, &_showPoints, "show_points", "Show Point Cloud");
    static const char* densityLabels[] = {"Low", "Medium", "High", nullptr};
    Enumeration_knob(f, &_pointDensity, densityLabels, "point_density", "Point Density");
    Double_knob(f, &_pointSize, "point_size", "Point Size");
    static const char* vpColorNames[] = {
        "Greyscale", "Heat", "Cool", "Blackbody", "Custom Gradient", nullptr
    };
    Enumeration_knob(f, &_viewportColor, vpColorNames, "viewport_color", "Viewport Color");
    Tooltip(f, "Color scheme for 3D viewport point cloud.");
}

int VDBRenderIop::knob_changed(Knob* k)
{
    if (k->is("file") || k->is("grid_name") || k->is("frame_offset")) {
        _gridValid = false;
        _previewPoints.clear();
        return 1;
    }
    if (k->is("show_points") || k->is("point_density")) {
        _previewPoints.clear();
        return 1;
    }
    return Iop::knob_changed(k);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Input handling
// ═══════════════════════════════════════════════════════════════════════════════

CameraOp* VDBRenderIop::camera() const
{ return dynamic_cast<CameraOp*>(Op::input(0)); }

AxisOp* VDBRenderIop::axisInput() const
{
    if (inputs() < 2 || input(1) == nullptr) return nullptr;
    return dynamic_cast<AxisOp*>(Op::input(1));
}

const char* VDBRenderIop::input_label(int idx, char*) const
{
    if (idx == 0) return "cam";
    if (idx == 1) return "axis";
    return nullptr;
}

bool VDBRenderIop::test_input(int idx, Op* op) const
{
    if (idx == 0) {
        if (dynamic_cast<CameraOp*>(op)) return true;
        return Iop::test_input(idx, op);
    }
    if (idx == 1) return dynamic_cast<AxisOp*>(op) != nullptr;
    return Iop::test_input(idx, op);
}

Op* VDBRenderIop::default_input(int idx) const
{
    if (idx == 1) return nullptr;
    return Iop::default_input(idx);
}

// ═══════════════════════════════════════════════════════════════════════════════
// Frame sequence
// ═══════════════════════════════════════════════════════════════════════════════

std::string VDBRenderIop::resolveFramePath(int frame) const
{
    std::string path(_vdbFilePath ? _vdbFilePath : "");
    if (path.empty()) return path;

    // #### patterns
    size_t h = path.find('#');
    if (h != std::string::npos) {
        size_t he = h;
        while (he < path.size() && path[he] == '#') ++he;
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%0*d", (int)(he-h), frame);
        path.replace(h, he-h, buf);
        return path;
    }
    // %0Nd patterns
    size_t p = path.find('%');
    if (p != std::string::npos) {
        size_t s = p+1;
        if (s < path.size() && path[s]=='0') ++s;
        while (s < path.size() && path[s]>='0' && path[s]<='9') ++s;
        if (s < path.size() && path[s]=='d') {
            std::string fmt = path.substr(p, s-p+1);
            char buf[64];
            std::snprintf(buf, sizeof(buf), fmt.c_str(), frame);
            path.replace(p, s-p+1, buf);
            return path;
        }
    }
    // Auto-detect last digit group
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) dot = path.size();
    size_t ne = dot, ns = ne;
    while (ns > 0 && path[ns-1]>='0' && path[ns-1]<='9') --ns;
    if (ns < ne) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%0*d", (int)(ne-ns), frame);
        path.replace(ns, ne-ns, buf);
        return path;
    }
    return path;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Hash
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::append(Hash& hash)
{
    hash.append(outputContext().frame());
    hash.append(_frameOffset);
    hash.append(_colorScheme);
    if (_hasVolumeXform)
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                hash.append(_volFwd[i][j]);
}

// ═══════════════════════════════════════════════════════════════════════════════
// 3D Viewport
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::build_handles(ViewerContext* ctx)
{
    if (!_gridValid) return;
    if (!_showBbox && !_showPoints) return;
    add_draw_handle(ctx);
}

void VDBRenderIop::rebuildPointCloud()
{
    _previewPoints.clear();
    _maxDensity = 1.0f;
    if (!_floatGrid) return;

    int res = 24;
    if (_pointDensity == 1) res = 40;
    if (_pointDensity == 2) res = 64;

    auto acc = _floatGrid->getConstAccessor();
    const auto& xf = _floatGrid->transform();
    const double dx = (_bboxMax[0]-_bboxMin[0]) / res;
    const double dy = (_bboxMax[1]-_bboxMin[1]) / res;
    const double dz = (_bboxMax[2]-_bboxMin[2]) / res;

    _previewPoints.reserve(res*res*res/4);
    float maxD = 0;

    for (int iz = 0; iz < res; ++iz) {
        double wz = _bboxMin[2] + (iz+0.5)*dz;
        for (int iy = 0; iy < res; ++iy) {
            double wy = _bboxMin[1] + (iy+0.5)*dy;
            for (int ix = 0; ix < res; ++ix) {
                double wx = _bboxMin[0] + (ix+0.5)*dx;
                auto iPos = xf.worldToIndex(openvdb::Vec3d(wx,wy,wz));
                openvdb::Coord ijk(
                    (int)std::floor(iPos[0]),
                    (int)std::floor(iPos[1]),
                    (int)std::floor(iPos[2]));
                float d = acc.getValue(ijk);
                if (d > 0.001f) {
                    float px=(float)wx, py=(float)wy, pz=(float)wz;
                    if (_hasVolumeXform) {
                        double tx = _volFwd[0][0]*wx+_volFwd[1][0]*wy+_volFwd[2][0]*wz+_volFwd[3][0];
                        double ty = _volFwd[0][1]*wx+_volFwd[1][1]*wy+_volFwd[2][1]*wz+_volFwd[3][1];
                        double tz = _volFwd[0][2]*wx+_volFwd[1][2]*wy+_volFwd[2][2]*wz+_volFwd[3][2];
                        px=(float)tx; py=(float)ty; pz=(float)tz;
                    }
                    _previewPoints.push_back({px,py,pz,d});
                    if (d > maxD) maxD = d;
                }
            }
        }
    }
    _maxDensity = (maxD > 0) ? maxD : 1.0f;
}

void VDBRenderIop::draw_handle(ViewerContext* ctx)
{
    if (!_gridValid) return;

    glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_ENABLE_BIT | GL_POINT_BIT);
    glDisable(GL_LIGHTING);

    // ── Bounding box ──
    if (_showBbox) {
        float corners[8][3];
        int ci = 0;
        for (int iz = 0; iz <= 1; ++iz)
            for (int iy = 0; iy <= 1; ++iy)
                for (int ix = 0; ix <= 1; ++ix) {
                    double wx = ix ? _bboxMax[0] : _bboxMin[0];
                    double wy = iy ? _bboxMax[1] : _bboxMin[1];
                    double wz = iz ? _bboxMax[2] : _bboxMin[2];
                    if (_hasVolumeXform) {
                        double tx=_volFwd[0][0]*wx+_volFwd[1][0]*wy+_volFwd[2][0]*wz+_volFwd[3][0];
                        double ty=_volFwd[0][1]*wx+_volFwd[1][1]*wy+_volFwd[2][1]*wz+_volFwd[3][1];
                        double tz=_volFwd[0][2]*wx+_volFwd[1][2]*wy+_volFwd[2][2]*wz+_volFwd[3][2];
                        wx=tx; wy=ty; wz=tz;
                    }
                    corners[ci][0]=(float)wx; corners[ci][1]=(float)wy; corners[ci][2]=(float)wz;
                    ++ci;
                }

        glLineWidth(1.5f);
        glColor3f(0,1,0);
        glBegin(GL_LINES);
        for (int e = 0; e < 12; ++e) {
            glVertex3fv(corners[_bboxEdges[e][0]]);
            glVertex3fv(corners[_bboxEdges[e][1]]);
        }
        glEnd();

        // Center cross
        float cx=0,cy=0,cz=0;
        for (int i=0;i<8;++i){cx+=corners[i][0];cy+=corners[i][1];cz+=corners[i][2];}
        cx/=8;cy/=8;cz/=8;
        float sz=0;
        for(int i=0;i<8;++i){float d=std::sqrt((corners[i][0]-cx)*(corners[i][0]-cx)+(corners[i][1]-cy)*(corners[i][1]-cy)+(corners[i][2]-cz)*(corners[i][2]-cz));if(d>sz)sz=d;}
        sz*=0.05f;
        glColor3f(1,1,0);
        glBegin(GL_LINES);
        glVertex3f(cx-sz,cy,cz);glVertex3f(cx+sz,cy,cz);
        glVertex3f(cx,cy-sz,cz);glVertex3f(cx,cy+sz,cz);
        glVertex3f(cx,cy,cz-sz);glVertex3f(cx,cy,cz+sz);
        glEnd();
    }

    // ── Point cloud ──
    if (_showPoints && _floatGrid) {
        int curFrame = (int)outputContext().frame() + _frameOffset;
        std::string curPath = resolveFramePath(curFrame);
        if (_previewPoints.empty() ||
            _cachedPointDensity != _pointDensity ||
            _cachedPointsPath != curPath ||
            _cachedPointsFrame != curFrame ||
            _cachedHasXform != _hasVolumeXform)
        {
            rebuildPointCloud();
            _cachedPointDensity = _pointDensity;
            _cachedPointsPath   = curPath;
            _cachedPointsFrame  = curFrame;
            _cachedHasXform     = _hasVolumeXform;
        }

        if (!_previewPoints.empty()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE);
            glPointSize((float)_pointSize);
            glEnable(GL_POINT_SMOOTH);

            // Map viewport color enum (0-based, no "Lit" option) to ColorScheme
            ColorScheme vpScheme;
            switch (_viewportColor) {
                default:
                case 0: vpScheme = kGreyscale; break;
                case 1: vpScheme = kHeat; break;
                case 2: vpScheme = kCool; break;
                case 3: vpScheme = kBlackbody; break;
                case 4: vpScheme = kCustomGradient; break;
            }

            float gA[3] = {(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};
            float gB[3] = {(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};
            float invMax = 1.0f / _maxDensity;

            glBegin(GL_POINTS);
            for (const auto& pt : _previewPoints) {
                float t = std::min(pt.density * invMax, 1.0f);
                Color3 c = evalRamp(vpScheme, t, gA, gB, _tempMin, _tempMax);
                float a = 0.15f + 0.85f * t;
                glColor4f(c.r, c.g, c.b, a);
                glVertex3f(pt.x, pt.y, pt.z);
            }
            glEnd();
            glDisable(GL_BLEND);
        }
    }

    glPopAttrib();
}

// ═══════════════════════════════════════════════════════════════════════════════
// _validate
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::_validate(bool for_real)
{
    _camValid = false;
    _hasVolumeXform = false;

    const Format* fmt = _formats.format();
    const Format* full = _formats.fullSizeFormat();
    if (!full) full = fmt;
    if (fmt) {
        info_.format(*fmt);
        info_.full_size_format(full ? *full : *fmt);
        info_.set(*fmt);
    }
    info_.channels(Mask_RGBA);
    set_out_channels(Mask_RGBA);
    info_.turn_on(Mask_RGBA);

    if (!for_real) return;

    // ── Camera ──
    if (CameraOp* cam = camera()) {
        cam->validate(for_real);
        const auto cw = cam->worldTransform();
        _camOrigin = openvdb::Vec3d(cw[3][0], cw[3][1], cw[3][2]);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                _camRot[c][r] = (double)cw[c][r];
        _halfW = (double)cam->horizontalAperture() * 0.5 / (double)cam->focalLength();
        _camValid = true;
    } else {
        error("Connect a Camera to input 0.");
        return;
    }

    // ── Axis ──
    if (AxisOp* axis = axisInput()) {
        axis->validate(for_real);
        const auto axM = axis->worldTransform();
        for (int c=0;c<4;++c) for (int r=0;r<4;++r) _volFwd[c][r]=(double)axM[c][r];
        DD::Image::Matrix4 m4;
        for (int c=0;c<4;++c) for (int r=0;r<4;++r) m4[c][r]=(float)_volFwd[c][r];
        auto inv = m4.inverse();
        for (int c=0;c<4;++c) for (int r=0;r<4;++r) _volInv[c][r]=(double)inv[c][r];
        _hasVolumeXform = true;
    } else {
        for (int i=0;i<4;++i) for(int j=0;j<4;++j) _volFwd[i][j]=_volInv[i][j]=(i==j)?1.0:0.0;
    }

    // ── Load VDB ──
    {
        Guard guard(_loadLock);
        int curFrame = (int)outputContext().frame() + _frameOffset;
        std::string path = resolveFramePath(curFrame);
        std::string grid(_gridName ? _gridName : "");

        // Fallback to original path
        {
            std::string tp = path; for(auto& c:tp)if(c=='\\')c='/';
            FILE* f = fopen(tp.c_str(),"rb");
            if (!f) {
                std::string orig(_vdbFilePath?_vdbFilePath:"");
                if (orig!=path) { for(auto& c:orig)if(c=='\\')c='/';
                    FILE* of=fopen(orig.c_str(),"rb");
                    if(of){fclose(of);path=orig;}}
            } else fclose(f);
        }

        if (!_gridValid || path!=_loadedPath || grid!=_loadedGrid || curFrame!=_loadedFrame) {
            _floatGrid.reset(); _gridValid=false; _previewPoints.clear();
            if (!path.empty()) {
                try {
                    std::string cp=path; for(auto& c:cp)if(c=='\\')c='/';
                    openvdb::io::File file(cp); file.open();
                    std::string target = grid.empty() ? "density" : grid;
                    openvdb::GridBase::Ptr bg; bool found=false;
                    for (auto it=file.beginName();it!=file.endName();++it)
                        if(it.gridName()==target){bg=file.readGrid(target);found=true;break;}
                    if(!found) for(auto it=file.beginName();it!=file.endName();++it){
                        auto g=file.readGrid(it.gridName());
                        if(g->isType<openvdb::FloatGrid>()){bg=g;found=true;break;}}
                    file.close();
                    if(!found||!bg){error("No float grid '%s' found.",target.c_str());return;}
                    if(!bg->isType<openvdb::FloatGrid>()){error("Grid is not float.");return;}

                    _floatGrid=openvdb::gridPtrCast<openvdb::FloatGrid>(bg);
                    auto ab=_floatGrid->evalActiveVoxelBoundingBox();
                    if(ab.empty()){error("No active voxels.");return;}

                    const auto& xf=_floatGrid->transform();
                    const auto& lo=ab.min(); const auto& hi=ab.max();
                    openvdb::Vec3d corners[8]; int ci=0;
                    for(int iz=0;iz<=1;++iz)for(int iy=0;iy<=1;++iy)for(int ix=0;ix<=1;++ix)
                        corners[ci++]=xf.indexToWorld(openvdb::Vec3d(ix?hi.x()+1.0:lo.x(),iy?hi.y()+1.0:lo.y(),iz?hi.z()+1.0:lo.z()));
                    _bboxMin=_bboxMax=corners[0];
                    for(int i=1;i<8;++i)for(int a=0;a<3;++a){
                        _bboxMin[a]=std::min(_bboxMin[a],corners[i][a]);
                        _bboxMax[a]=std::max(_bboxMax[a],corners[i][a]);}

                    _gridValid=true; _loadedPath=path; _loadedGrid=grid; _loadedFrame=curFrame;
                } catch(const std::exception& e){error("OpenVDB: %s",e.what());}
                  catch(...){error("OpenVDB: unknown error.");}
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// _request / engine
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::_request(int,int,int,int,ChannelMask,int) {}

void VDBRenderIop::engine(int y, int x, int r, ChannelMask channels, Row& row)
{
    if (!_gridValid || !_floatGrid || !_camValid) {
        foreach(z,channels){float*p=row.writable(z);for(int i=x;i<r;++i)p[i]=0;}
        return;
    }

    const Format& fmt = format();
    const int W=fmt.width(), H=fmt.height();
    const double halfW=_halfW;
    const double halfH=_halfW*(double)H/(double)W;

    float* rOut=row.writable(Chan_Red);
    float* gOut=row.writable(Chan_Green);
    float* bOut=row.writable(Chan_Blue);
    float* aOut=row.writable(Chan_Alpha);

    ColorScheme scheme = static_cast<ColorScheme>(_colorScheme);
    float gA[3]={(float)_gradStart[0],(float)_gradStart[1],(float)_gradStart[2]};
    float gB[3]={(float)_gradEnd[0],(float)_gradEnd[1],(float)_gradEnd[2]};

    for (int ix = x; ix < r; ++ix)
    {
        double ndcX = (ix+0.5)/(double)W*2.0-1.0;
        double ndcY = (y +0.5)/(double)H*2.0-1.0;
        double rcx=ndcX*halfW, rcy=ndcY*halfH, rcz=-1.0;

        double rdx=_camRot[0][0]*rcx+_camRot[1][0]*rcy+_camRot[2][0]*rcz;
        double rdy=_camRot[0][1]*rcx+_camRot[1][1]*rcy+_camRot[2][1]*rcz;
        double rdz=_camRot[0][2]*rcx+_camRot[1][2]*rcy+_camRot[2][2]*rcz;
        double len=std::sqrt(rdx*rdx+rdy*rdy+rdz*rdz);
        if(len>1e-8){rdx/=len;rdy/=len;rdz/=len;}

        double ox=_camOrigin[0],oy=_camOrigin[1],oz=_camOrigin[2];
        if (_hasVolumeXform) {
            double tx=_volInv[0][0]*ox+_volInv[1][0]*oy+_volInv[2][0]*oz+_volInv[3][0];
            double ty=_volInv[0][1]*ox+_volInv[1][1]*oy+_volInv[2][1]*oz+_volInv[3][1];
            double tz=_volInv[0][2]*ox+_volInv[1][2]*oy+_volInv[2][2]*oz+_volInv[3][2];
            ox=tx;oy=ty;oz=tz;
            double dx2=_volInv[0][0]*rdx+_volInv[1][0]*rdy+_volInv[2][0]*rdz;
            double dy2=_volInv[0][1]*rdx+_volInv[1][1]*rdy+_volInv[2][1]*rdz;
            double dz2=_volInv[0][2]*rdx+_volInv[1][2]*rdy+_volInv[2][2]*rdz;
            len=std::sqrt(dx2*dx2+dy2*dy2+dz2*dz2);
            if(len>1e-8){dx2/=len;dy2/=len;dz2/=len;}
            rdx=dx2;rdy=dy2;rdz=dz2;
        }

        openvdb::Vec3d rayO(ox,oy,oz), rayD(rdx,rdy,rdz);

        if (scheme == kLit) {
            float R=0,G=0,B=0,A=0;
            marchRay(rayO,rayD,R,G,B,A);
            rOut[ix]=R; gOut[ix]=G; bOut[ix]=B; aOut[ix]=A;
        } else {
            float density=0, alpha=0;
            marchRayDensity(rayO,rayD,density,alpha);
            Color3 c = evalRamp(scheme, density, gA, gB, _tempMin, _tempMax);
            rOut[ix] = c.r * alpha;
            gOut[ix] = c.g * alpha;
            bOut[ix] = c.b * alpha;
            aOut[ix] = alpha;
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════
// marchRay — lit mode (single-scatter)
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::marchRay(const openvdb::Vec3d& origin,
                             const openvdb::Vec3d& dir,
                             float& outR, float& outG,
                             float& outB, float& outA) const
{
    outR=outG=outB=outA=0;
    double tEnter=0,tExit=1e9;
    for(int a=0;a<3;++a){
        double inv=(std::abs(dir[a])>1e-8)?1.0/dir[a]:1e38;
        double t0=(_bboxMin[a]-origin[a])*inv, t1=(_bboxMax[a]-origin[a])*inv;
        if(t0>t1)std::swap(t0,t1);
        tEnter=std::max(tEnter,t0); tExit=std::min(tExit,t1);
    }
    if(tEnter>=tExit||tExit<=0)return;

    auto acc=_floatGrid->getConstAccessor();
    const auto& xf=_floatGrid->transform();
    openvdb::Vec3d lDir(_lightDir[0],_lightDir[1],_lightDir[2]);
    {double l=std::sqrt(lDir[0]*lDir[0]+lDir[1]*lDir[1]+lDir[2]*lDir[2]);if(l>1e-8)lDir*=1.0/l;}

    double step=_stepSize, ext=_extinction, scat=_scattering;
    double T=1.0, aR=0,aG=0,aB=0;

    for(double t=tEnter; t<tExit && T>0.001; t+=step){
        auto wPos=origin+t*dir;
        auto iPos=xf.worldToIndex(wPos);
        openvdb::Coord ijk((int)std::floor(iPos[0]),(int)std::floor(iPos[1]),(int)std::floor(iPos[2]));
        float d=acc.getValue(ijk);
        if(d>1e-6f){
            double lightT=1.0;
            {auto la=_floatGrid->getConstAccessor();
             double ls=step*4; double lt=0;
             for(int i=0;i<16;++i){
                auto lw=wPos+lt*lDir; bool in=true;
                for(int a=0;a<3;++a)if(lw[a]<_bboxMin[a]||lw[a]>_bboxMax[a]){in=false;break;}
                if(!in)break;
                auto li=xf.worldToIndex(lw);
                openvdb::Coord lc((int)std::floor(li[0]),(int)std::floor(li[1]),(int)std::floor(li[2]));
                lightT*=std::exp(-la.getValue(lc)*ext*ls); lt+=ls;}}

            double ss=d*scat, se=d*ext;
            double contrib=ss*lightT*T*step;
            aR+=contrib*_lightColor[0]; aG+=contrib*_lightColor[1]; aB+=contrib*_lightColor[2];
            T*=std::exp(-se*step);
        }
    }
    outR=(float)aR; outG=(float)aG; outB=(float)aB; outA=(float)(1.0-T);
}

// ═══════════════════════════════════════════════════════════════════════════════
// marchRayDensity — returns normalised accumulated density + alpha
// ═══════════════════════════════════════════════════════════════════════════════

void VDBRenderIop::marchRayDensity(const openvdb::Vec3d& origin,
                                    const openvdb::Vec3d& dir,
                                    float& outDensity, float& outAlpha) const
{
    outDensity=0; outAlpha=0;
    double tEnter=0,tExit=1e9;
    for(int a=0;a<3;++a){
        double inv=(std::abs(dir[a])>1e-8)?1.0/dir[a]:1e38;
        double t0=(_bboxMin[a]-origin[a])*inv, t1=(_bboxMax[a]-origin[a])*inv;
        if(t0>t1)std::swap(t0,t1);
        tEnter=std::max(tEnter,t0); tExit=std::min(tExit,t1);
    }
    if(tEnter>=tExit||tExit<=0)return;

    auto acc=_floatGrid->getConstAccessor();
    const auto& xf=_floatGrid->transform();
    double step=_stepSize, ext=_extinction;
    double T=1.0;
    double weightedDensity=0;

    for(double t=tEnter; t<tExit && T>0.001; t+=step){
        auto wPos=origin+t*dir;
        auto iPos=xf.worldToIndex(wPos);
        openvdb::Coord ijk((int)std::floor(iPos[0]),(int)std::floor(iPos[1]),(int)std::floor(iPos[2]));
        float d=acc.getValue(ijk);
        if(d>1e-6f){
            double se=d*ext;
            double absorbed = T * (1.0 - std::exp(-se*step));
            // Transmittance-weighted density: front voxels contribute more
            weightedDensity += d * absorbed;
            T *= std::exp(-se*step);
        }
    }

    double alpha = 1.0 - T;
    // Normalise density by alpha to get average front-weighted density
    outDensity = (alpha > 1e-6) ? std::min((float)(weightedDensity / alpha), 1.0f) : 0.0f;
    outAlpha = (float)alpha;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Registration
// ═══════════════════════════════════════════════════════════════════════════════

static Op* build(Node* node) { return new VDBRenderIop(node); }
const Op::Description VDBRenderIop::desc(VDBRenderIop::CLASS, build);
