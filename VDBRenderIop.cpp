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
    "Input 0: Camera node (required)\n"
    "Input 1: Axis/Transform node (optional — moves the volume in 3D)";

bool VDBRenderIop::_vdbInitialised = false;

// ─── Construction ────────────────────────────────────────────────────────────

VDBRenderIop::VDBRenderIop(Node* node) : Iop(node)
{
    inputs(2);          // cam + optional axis
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            _camRot[c][r] = (c == r) ? 1.0 : 0.0;
    // Identity volume transform
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            _volFwd[i][j] = _volInv[i][j] = (i == j) ? 1.0 : 0.0;

    if (!_vdbInitialised) {
        openvdb::initialize();
        _vdbInitialised = true;
    }
}

// ─── Knobs ───────────────────────────────────────────────────────────────────

void VDBRenderIop::knobs(Knob_Callback f)
{
    Text_knob(f, "<b>VDBRender</b> — OpenVDB Ray Marcher\n"
                 "Input 0: Camera  |  Input 1: Axis (optional, moves volume)");
    Format_knob(f, &_formats, "format", "Output Format");

    Divider(f, "File");
    File_knob(f, &_vdbFilePath, "file", "VDB File");
    Tooltip(f, "Path to a .vdb file.\n\n"
               "Sequences auto-detected:\n"
               "  dust_impact_0099.vdb → replaces frame number automatically\n"
               "  smoke.####.vdb or smoke.%04d.vdb also supported");
    String_knob(f, &_gridName, "grid_name", "Grid Name");
    Tooltip(f, "Grid name inside the VDB (default: 'density').");
    Int_knob(f, &_frameOffset, "frame_offset", "Frame Offset");
    Tooltip(f, "Offset added to current frame for sequences.");

    Divider(f, "Ray March");
    Double_knob(f, &_stepSize,   "step_size",  "Step Size");
    Tooltip(f, "World-space step size. Smaller = more detail, slower.\n"
               "Start with 0.5 and reduce for final renders.");
    Double_knob(f, &_extinction, "extinction", "Extinction");
    Tooltip(f, "How quickly light is absorbed. Higher = denser.");
    Double_knob(f, &_scattering, "scattering", "Scattering");
    Tooltip(f, "How much light scatters. Higher = brighter.");

    Divider(f, "Lighting");
    XYZ_knob(f,   _lightDir,   "light_dir",   "Light Direction");
    Color_knob(f, _lightColor, "light_color", "Light Color");

    Divider(f, "3D Viewport");
    Bool_knob(f, &_showBbox, "show_bbox", "Show Bounding Box");
    Tooltip(f, "Green wireframe in 3D viewer.");
    Bool_knob(f, &_showPoints, "show_points", "Show Point Cloud");
    Tooltip(f, "Density preview as colored points in 3D viewer.");
    static const char* densityLabels[] = {"Low", "Medium", "High", nullptr};
    Enumeration_knob(f, &_pointDensity, densityLabels, "point_density", "Point Density");
    Tooltip(f, "Low ~16k, Medium ~64k, High ~250k points.");
    Double_knob(f, &_pointSize, "point_size", "Point Size");
    Tooltip(f, "GL point size for the density preview.");
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

// ─── Input handling ──────────────────────────────────────────────────────────

CameraOp* VDBRenderIop::camera() const
{
    return dynamic_cast<CameraOp*>(Op::input(0));
}

AxisOp* VDBRenderIop::axisInput() const
{
    if (inputs() < 2 || input(1) == nullptr) return nullptr;
    return dynamic_cast<AxisOp*>(Op::input(1));
}

const char* VDBRenderIop::input_label(int idx, char* buf) const
{
    if (idx == 0) return "cam";
    if (idx == 1) return "axis";
    return Iop::input_label(idx, buf);
}

bool VDBRenderIop::test_input(int idx, Op* op) const
{
    if (idx == 0) {
        if (dynamic_cast<CameraOp*>(op)) return true;
        return Iop::test_input(idx, op);
    }
    if (idx == 1) {
        if (dynamic_cast<AxisOp*>(op)) return true;
        return false;  // only accept Axis types
    }
    return Iop::test_input(idx, op);
}

Op* VDBRenderIop::default_input(int idx) const
{
    if (idx == 1) return nullptr;  // axis input is optional
    return Iop::default_input(idx);
}

// ─── Frame sequence helper ───────────────────────────────────────────────────

std::string VDBRenderIop::resolveFramePath(int frame) const
{
    std::string path(_vdbFilePath ? _vdbFilePath : "");
    if (path.empty()) return path;

    // Method 1: #### patterns
    size_t hashStart = path.find('#');
    if (hashStart != std::string::npos) {
        size_t hashEnd = hashStart;
        while (hashEnd < path.size() && path[hashEnd] == '#') ++hashEnd;
        int padding = static_cast<int>(hashEnd - hashStart);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%0*d", padding, frame);
        path.replace(hashStart, hashEnd - hashStart, buf);
        return path;
    }

    // Method 2: %0Nd printf patterns
    size_t pct = path.find('%');
    if (pct != std::string::npos) {
        size_t scan = pct + 1;
        if (scan < path.size() && path[scan] == '0') ++scan;
        while (scan < path.size() && path[scan] >= '0' && path[scan] <= '9') ++scan;
        if (scan < path.size() && path[scan] == 'd') {
            std::string fmt = path.substr(pct, scan - pct + 1);
            char buf[64];
            std::snprintf(buf, sizeof(buf), fmt.c_str(), frame);
            path.replace(pct, scan - pct + 1, buf);
            return path;
        }
    }

    // Method 3: Auto-detect last digit group before extension
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) dot = path.size();
    size_t numEnd = dot;
    size_t numStart = numEnd;
    while (numStart > 0 && path[numStart - 1] >= '0' && path[numStart - 1] <= '9')
        --numStart;
    if (numStart < numEnd) {
        int padding = static_cast<int>(numEnd - numStart);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%0*d", padding, frame);
        path.replace(numStart, numEnd - numStart, buf);
        return path;
    }

    return path;
}

// ─── append (frame + transform sensitivity) ──────────────────────────────────

void VDBRenderIop::append(Hash& hash)
{
    hash.append(outputContext().frame());
    hash.append(_frameOffset);
    // Include axis transform so we re-render when it moves
    if (_hasVolumeXform) {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                hash.append(_volFwd[i][j]);
    }
}

// ─── 3D Viewport ─────────────────────────────────────────────────────────────

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
    const auto& xform = _floatGrid->transform();

    const double dx = (_bboxMax[0] - _bboxMin[0]) / res;
    const double dy = (_bboxMax[1] - _bboxMin[1]) / res;
    const double dz = (_bboxMax[2] - _bboxMin[2]) / res;
    const float threshold = 0.001f;

    _previewPoints.reserve(res * res * res / 4);
    float maxD = 0.0f;

    for (int iz = 0; iz < res; ++iz) {
        double wz = _bboxMin[2] + (iz + 0.5) * dz;
        for (int iy = 0; iy < res; ++iy) {
            double wy = _bboxMin[1] + (iy + 0.5) * dy;
            for (int ix = 0; ix < res; ++ix) {
                double wx = _bboxMin[0] + (ix + 0.5) * dx;

                openvdb::Vec3d iPos = xform.worldToIndex(openvdb::Vec3d(wx, wy, wz));
                openvdb::Coord ijk(
                    static_cast<int>(std::floor(iPos[0])),
                    static_cast<int>(std::floor(iPos[1])),
                    static_cast<int>(std::floor(iPos[2])));
                float d = acc.getValue(ijk);

                if (d > threshold) {
                    // If volume has a transform, apply it to get world position
                    float px = static_cast<float>(wx);
                    float py = static_cast<float>(wy);
                    float pz = static_cast<float>(wz);
                    if (_hasVolumeXform) {
                        double tx = _volFwd[0][0]*wx + _volFwd[1][0]*wy + _volFwd[2][0]*wz + _volFwd[3][0];
                        double ty = _volFwd[0][1]*wx + _volFwd[1][1]*wy + _volFwd[2][1]*wz + _volFwd[3][1];
                        double tz = _volFwd[0][2]*wx + _volFwd[1][2]*wy + _volFwd[2][2]*wz + _volFwd[3][2];
                        px = static_cast<float>(tx);
                        py = static_cast<float>(ty);
                        pz = static_cast<float>(tz);
                    }
                    _previewPoints.push_back({px, py, pz, d});
                    if (d > maxD) maxD = d;
                }
            }
        }
    }
    _maxDensity = (maxD > 0.0f) ? maxD : 1.0f;
}

void VDBRenderIop::draw_handle(ViewerContext* ctx)
{
    if (!_gridValid) return;

    glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_ENABLE_BIT | GL_POINT_BIT);
    glDisable(GL_LIGHTING);

    // ── Bounding box ──
    if (_showBbox) {
        // Build corners in VDB-local space, optionally transform to world
        float corners[8][3];
        int ci = 0;
        for (int iz = 0; iz <= 1; ++iz)
            for (int iy = 0; iy <= 1; ++iy)
                for (int ix = 0; ix <= 1; ++ix) {
                    double wx = ix ? _bboxMax[0] : _bboxMin[0];
                    double wy = iy ? _bboxMax[1] : _bboxMin[1];
                    double wz = iz ? _bboxMax[2] : _bboxMin[2];
                    if (_hasVolumeXform) {
                        double tx = _volFwd[0][0]*wx + _volFwd[1][0]*wy + _volFwd[2][0]*wz + _volFwd[3][0];
                        double ty = _volFwd[0][1]*wx + _volFwd[1][1]*wy + _volFwd[2][1]*wz + _volFwd[3][1];
                        double tz = _volFwd[0][2]*wx + _volFwd[1][2]*wy + _volFwd[2][2]*wz + _volFwd[3][2];
                        wx = tx; wy = ty; wz = tz;
                    }
                    corners[ci][0] = static_cast<float>(wx);
                    corners[ci][1] = static_cast<float>(wy);
                    corners[ci][2] = static_cast<float>(wz);
                    ++ci;
                }

        glLineWidth(1.5f);
        glColor3f(0.0f, 1.0f, 0.0f);
        glBegin(GL_LINES);
        for (int e = 0; e < 12; ++e) {
            glVertex3fv(corners[_bboxEdges[e][0]]);
            glVertex3fv(corners[_bboxEdges[e][1]]);
        }
        glEnd();

        // Center cross
        float cx = 0, cy = 0, cz = 0;
        for (int i = 0; i < 8; ++i) { cx += corners[i][0]; cy += corners[i][1]; cz += corners[i][2]; }
        cx /= 8; cy /= 8; cz /= 8;
        float sz = 0;
        for (int i = 0; i < 8; ++i) {
            float d = std::sqrt((corners[i][0]-cx)*(corners[i][0]-cx) +
                                (corners[i][1]-cy)*(corners[i][1]-cy) +
                                (corners[i][2]-cz)*(corners[i][2]-cz));
            if (d > sz) sz = d;
        }
        sz *= 0.05f;

        glColor3f(1.0f, 1.0f, 0.0f);
        glBegin(GL_LINES);
        glVertex3f(cx-sz,cy,cz); glVertex3f(cx+sz,cy,cz);
        glVertex3f(cx,cy-sz,cz); glVertex3f(cx,cy+sz,cz);
        glVertex3f(cx,cy,cz-sz); glVertex3f(cx,cy,cz+sz);
        glEnd();
    }

    // ── Density point cloud ──
    if (_showPoints && _floatGrid) {
        int curFrame = static_cast<int>(outputContext().frame()) + _frameOffset;
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
            glPointSize(static_cast<float>(_pointSize));
            glEnable(GL_POINT_SMOOTH);

            glBegin(GL_POINTS);
            float invMax = 1.0f / _maxDensity;
            for (const auto& pt : _previewPoints) {
                float t = std::min(pt.density * invMax, 1.0f);
                float r = 0.2f + 0.8f * t;
                float g = 0.15f + 0.85f * t;
                float b = 0.05f + 0.55f * t;
                float a = 0.15f + 0.85f * t;
                glColor4f(r, g, b, a);
                glVertex3f(pt.x, pt.y, pt.z);
            }
            glEnd();
            glDisable(GL_BLEND);
        }
    }

    glPopAttrib();
}

// ─── _validate ───────────────────────────────────────────────────────────────

void VDBRenderIop::_validate(bool for_real)
{
    _camValid = false;
    _hasVolumeXform = false;

    // Use proxy-aware format from the FormatPair knob
    const Format* fmt = _formats.format();         // proxy format
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
                _camRot[c][r] = static_cast<double>(cw[c][r]);
        const double fl = static_cast<double>(cam->focalLength());
        const double fw = static_cast<double>(cam->horizontalAperture());
        _halfW    = (fw * 0.5) / fl;
        _camValid = true;
    } else {
        error("Connect a Camera node to input 0.");
        return;
    }

    // ── Axis transform (input 1, optional) ──
    if (AxisOp* axis = axisInput()) {
        axis->validate(for_real);
        const auto axM = axis->worldTransform();
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                _volFwd[c][r] = static_cast<double>(axM[c][r]);

        // Compute inverse (for transforming rays into volume space)
        // Use Nuke's Matrix4 for inversion
        DD::Image::Matrix4 m4;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                m4[c][r] = static_cast<float>(_volFwd[c][r]);
        DD::Image::Matrix4 inv = m4.inverse();
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                _volInv[c][r] = static_cast<double>(inv[c][r]);

        _hasVolumeXform = true;
    } else {
        // Identity
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                _volFwd[i][j] = _volInv[i][j] = (i == j) ? 1.0 : 0.0;
    }

    // ── Load VDB grid ──
    {
        Guard guard(_loadLock);

        int curFrame = static_cast<int>(outputContext().frame()) + _frameOffset;
        std::string path = resolveFramePath(curFrame);
        std::string grid(_gridName ? _gridName : "");

        // Fallback: if resolved path doesn't exist, try original
        {
            std::string testPath = path;
            for (char& ch : testPath) if (ch == '\\') ch = '/';
            FILE* test = fopen(testPath.c_str(), "rb");
            if (!test) {
                std::string origPath(_vdbFilePath ? _vdbFilePath : "");
                if (origPath != path) {
                    std::string origClean = origPath;
                    for (char& ch : origClean) if (ch == '\\') ch = '/';
                    FILE* origTest = fopen(origClean.c_str(), "rb");
                    if (origTest) { fclose(origTest); path = origPath; }
                }
            } else {
                fclose(test);
            }
        }

        if (!_gridValid || path != _loadedPath || grid != _loadedGrid || curFrame != _loadedFrame) {
            _floatGrid.reset();
            _gridValid = false;
            _previewPoints.clear();

            if (!path.empty()) {
                try {
                    std::string cleanPath = path;
                    for (char& ch : cleanPath) if (ch == '\\') ch = '/';

                    openvdb::io::File file(cleanPath);
                    file.open();

                    std::string targetName = grid.empty() ? "density" : grid;
                    openvdb::GridBase::Ptr baseGrid;
                    bool found = false;

                    for (auto it = file.beginName(); it != file.endName(); ++it) {
                        if (it.gridName() == targetName) {
                            baseGrid = file.readGrid(targetName);
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        for (auto it = file.beginName(); it != file.endName(); ++it) {
                            auto g = file.readGrid(it.gridName());
                            if (g->isType<openvdb::FloatGrid>()) {
                                baseGrid = g; found = true; break;
                            }
                        }
                    }
                    file.close();

                    if (!found || !baseGrid) {
                        error("No float grid '%s' found.", targetName.c_str());
                        return;
                    }
                    if (!baseGrid->isType<openvdb::FloatGrid>()) {
                        error("Grid '%s' is not a float grid.", targetName.c_str());
                        return;
                    }

                    _floatGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);

                    openvdb::CoordBBox activeBBox = _floatGrid->evalActiveVoxelBoundingBox();
                    if (activeBBox.empty()) { error("Grid has no active voxels."); return; }

                    const openvdb::math::Transform& xf = _floatGrid->transform();
                    openvdb::Vec3d corners[8];
                    const openvdb::Coord& lo = activeBBox.min();
                    const openvdb::Coord& hi = activeBBox.max();
                    int ci2 = 0;
                    for (int iz = 0; iz <= 1; ++iz)
                        for (int iy = 0; iy <= 1; ++iy)
                            for (int ix = 0; ix <= 1; ++ix)
                                corners[ci2++] = xf.indexToWorld(openvdb::Vec3d(
                                    ix ? hi.x()+1.0 : lo.x(),
                                    iy ? hi.y()+1.0 : lo.y(),
                                    iz ? hi.z()+1.0 : lo.z()));

                    _bboxMin = _bboxMax = corners[0];
                    for (int i = 1; i < 8; ++i)
                        for (int ax = 0; ax < 3; ++ax) {
                            _bboxMin[ax] = std::min(_bboxMin[ax], corners[i][ax]);
                            _bboxMax[ax] = std::max(_bboxMax[ax], corners[i][ax]);
                        }

                    _gridValid   = true;
                    _loadedPath  = path;
                    _loadedGrid  = grid;
                    _loadedFrame = curFrame;

                } catch (const std::exception& e) {
                    error("OpenVDB: %s", e.what());
                } catch (...) {
                    error("OpenVDB: unknown exception.");
                }
            }
        }
    }
}

// ─── _request ────────────────────────────────────────────────────────────────

void VDBRenderIop::_request(int x, int y, int r, int t,
                             ChannelMask channels, int count)
{
}

// ─── engine ──────────────────────────────────────────────────────────────────

void VDBRenderIop::engine(int y, int x, int r,
                           ChannelMask channels, Row& row)
{
    if (!_gridValid || !_floatGrid || !_camValid) {
        foreach (z, channels) {
            float* ptr = row.writable(z);
            for (int ix = x; ix < r; ++ix) ptr[ix] = 0.0f;
        }
        return;
    }

    const Format& fmt = format();
    const int W = fmt.width();
    const int H = fmt.height();
    const double halfW = _halfW;
    const double halfH = _halfW * static_cast<double>(H) / static_cast<double>(W);

    float* rOut = row.writable(Chan_Red);
    float* gOut = row.writable(Chan_Green);
    float* bOut = row.writable(Chan_Blue);
    float* aOut = row.writable(Chan_Alpha);

    for (int ix = x; ix < r; ++ix)
    {
        const double ndcX = (ix + 0.5) / static_cast<double>(W) * 2.0 - 1.0;
        const double ndcY = (y  + 0.5) / static_cast<double>(H) * 2.0 - 1.0;
        const double rcx =  ndcX * halfW;
        const double rcy =  ndcY * halfH;
        const double rcz = -1.0;

        // World-space ray direction
        double rdx = _camRot[0][0]*rcx + _camRot[1][0]*rcy + _camRot[2][0]*rcz;
        double rdy = _camRot[0][1]*rcx + _camRot[1][1]*rcy + _camRot[2][1]*rcz;
        double rdz = _camRot[0][2]*rcx + _camRot[1][2]*rcy + _camRot[2][2]*rcz;

        double len = std::sqrt(rdx*rdx + rdy*rdy + rdz*rdz);
        if (len > 1e-8) { rdx /= len; rdy /= len; rdz /= len; }

        // If volume has a transform, transform ray into volume-local space
        double ox = _camOrigin[0], oy = _camOrigin[1], oz = _camOrigin[2];
        if (_hasVolumeXform) {
            // Transform origin
            double tx = _volInv[0][0]*ox + _volInv[1][0]*oy + _volInv[2][0]*oz + _volInv[3][0];
            double ty = _volInv[0][1]*ox + _volInv[1][1]*oy + _volInv[2][1]*oz + _volInv[3][1];
            double tz = _volInv[0][2]*ox + _volInv[1][2]*oy + _volInv[2][2]*oz + _volInv[3][2];
            ox = tx; oy = ty; oz = tz;
            // Transform direction (no translation)
            double dx2 = _volInv[0][0]*rdx + _volInv[1][0]*rdy + _volInv[2][0]*rdz;
            double dy2 = _volInv[0][1]*rdx + _volInv[1][1]*rdy + _volInv[2][1]*rdz;
            double dz2 = _volInv[0][2]*rdx + _volInv[1][2]*rdy + _volInv[2][2]*rdz;
            len = std::sqrt(dx2*dx2 + dy2*dy2 + dz2*dz2);
            if (len > 1e-8) { dx2 /= len; dy2 /= len; dz2 /= len; }
            rdx = dx2; rdy = dy2; rdz = dz2;
        }

        openvdb::Vec3d rayOrigin(ox, oy, oz);
        openvdb::Vec3d rayDir(rdx, rdy, rdz);

        float R = 0, G = 0, B = 0, A = 0;
        marchRay(rayOrigin, rayDir, R, G, B, A);

        rOut[ix] = R;
        gOut[ix] = G;
        bOut[ix] = B;
        aOut[ix] = A;
    }
}

// ─── marchRay ────────────────────────────────────────────────────────────────

void VDBRenderIop::marchRay(const openvdb::Vec3d& origin,
                             const openvdb::Vec3d& dir,
                             float& outR, float& outG,
                             float& outB, float& outA) const
{
    outR = outG = outB = outA = 0.0f;

    // AABB slab test
    double tEnter = 0.0, tExit = 1e9;
    for (int axis = 0; axis < 3; ++axis) {
        double invD = (std::abs(dir[axis]) > 1e-8) ? 1.0 / dir[axis] : 1e38;
        double t0 = (_bboxMin[axis] - origin[axis]) * invD;
        double t1 = (_bboxMax[axis] - origin[axis]) * invD;
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit  = std::min(tExit,  t1);
    }
    if (tEnter >= tExit || tExit <= 0.0) return;

    auto acc = _floatGrid->getConstAccessor();
    const openvdb::math::Transform& xform = _floatGrid->transform();

    openvdb::Vec3d lDir(_lightDir[0], _lightDir[1], _lightDir[2]);
    { double l = std::sqrt(lDir[0]*lDir[0]+lDir[1]*lDir[1]+lDir[2]*lDir[2]);
      if (l > 1e-8) lDir *= 1.0/l; }

    const double step       = _stepSize;
    const double extinction = _extinction;
    const double scattering = _scattering;
    double transmittance    = 1.0;
    double accR = 0, accG = 0, accB = 0;

    double t = tEnter;
    while (t < tExit && transmittance > 0.001)
    {
        openvdb::Vec3d wPos = origin + t * dir;
        openvdb::Vec3d iPos = xform.worldToIndex(wPos);
        openvdb::Coord ijk(
            static_cast<int>(std::floor(iPos[0])),
            static_cast<int>(std::floor(iPos[1])),
            static_cast<int>(std::floor(iPos[2])));
        float density = acc.getValue(ijk);

        if (density > 1e-6f)
        {
            // Shadow ray (16 coarse steps)
            double lightT = 1.0;
            {
                auto lAcc = _floatGrid->getConstAccessor();
                const double lightStep = step * 4.0;
                double lt = 0.0;
                for (int ls = 0; ls < 16; ++ls) {
                    openvdb::Vec3d lWPos = wPos + lt * lDir;
                    bool inside = true;
                    for (int ax = 0; ax < 3; ++ax)
                        if (lWPos[ax] < _bboxMin[ax] || lWPos[ax] > _bboxMax[ax])
                        { inside = false; break; }
                    if (!inside) break;
                    openvdb::Vec3d lIPos = xform.worldToIndex(lWPos);
                    openvdb::Coord lijk(
                        static_cast<int>(std::floor(lIPos[0])),
                        static_cast<int>(std::floor(lIPos[1])),
                        static_cast<int>(std::floor(lIPos[2])));
                    lightT *= std::exp(-lAcc.getValue(lijk) * extinction * lightStep);
                    lt += lightStep;
                }
            }

            double sigma_s = density * scattering;
            double sigma_e = density * extinction;
            double contrib = sigma_s * lightT * transmittance * step;
            accR += contrib * _lightColor[0];
            accG += contrib * _lightColor[1];
            accB += contrib * _lightColor[2];
            transmittance *= std::exp(-sigma_e * step);
        }
        t += step;
    }

    outR = static_cast<float>(accR);
    outG = static_cast<float>(accG);
    outB = static_cast<float>(accB);
    outA = static_cast<float>(1.0 - transmittance);
}

// ─── Registration ────────────────────────────────────────────────────────────

static Op* build(Node* node) { return new VDBRenderIop(node); }
const Op::Description VDBRenderIop::desc(VDBRenderIop::CLASS, build);
