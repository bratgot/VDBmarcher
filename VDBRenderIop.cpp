#include "VDBRenderIop.h"
#include <DDImage/Knob.h>
#include <DDImage/Format.h>
#include <cmath>
#include <algorithm>
#include <cstdio>

using namespace DD::Image;

const char* VDBRenderIop::CLASS = "VDBRender";
const char* VDBRenderIop::HELP  =
    "Ray-march an OpenVDB density grid into RGBA.\n"
    "Reads .vdb files directly — no NanoVDB conversion needed.\n"
    "Connect a Camera node to input 0.";

bool VDBRenderIop::_vdbInitialised = false;

// ─── Construction ────────────────────────────────────────────────────────────

VDBRenderIop::VDBRenderIop(Node* node) : Iop(node)
{
    inputs(1);
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            _camRot[c][r] = (c == r) ? 1.0 : 0.0;

    if (!_vdbInitialised) {
        openvdb::initialize();
        _vdbInitialised = true;
    }
}

// ─── Knobs ───────────────────────────────────────────────────────────────────

void VDBRenderIop::knobs(Knob_Callback f)
{
    Text_knob(f, "Connect a <b>Camera</b> node to this node's input.\n"
                 "Point the camera at your VDB volume to render it.");
    Format_knob(f, &_formats, "format", "Output Format");
    Tooltip(f, "Resolution of the rendered output.");
    Divider(f, "File");
    File_knob(f, &_vdbFilePath, "file", "VDB File");
    Tooltip(f, "Path to a .vdb file. Supports any OpenVDB float grid.\n\n"
               "Sequences are detected automatically:\n"
               "  dust_impact_0099.vdb → auto-detects '0099' as frame number\n"
               "  Just pick any file from the sequence and scrub the timeline.\n\n"
               "You can also use explicit patterns:\n"
               "  smoke.####.vdb  →  smoke.0001.vdb, smoke.0002.vdb...\n"
               "  smoke.%04d.vdb  →  smoke.0001.vdb, smoke.0002.vdb...");
    String_knob(f, &_gridName, "grid_name", "Grid Name");
    Tooltip(f, "Name of the density grid inside the VDB file.\n"
               "Default: 'density'. Leave empty to use the first float grid found.");
    Int_knob(f, &_frameOffset, "frame_offset", "Frame Offset");
    Tooltip(f, "Offset added to the current frame number.\n"
               "Use this to shift the sequence timing.\n"
               "e.g. -10 to start the sequence 10 frames earlier.");

    Divider(f, "Ray March");
    Double_knob(f, &_stepSize,   "step_size",  "Step Size");
    Tooltip(f, "World-space step size for ray marching.\n"
               "Smaller = more detail but slower.\n"
               "Try 0.5-1.0 for large volumes, 0.01-0.05 for small ones.");
    Double_knob(f, &_extinction, "extinction", "Extinction");
    Tooltip(f, "Controls how quickly light is absorbed.\n"
               "Higher = denser/more opaque volume.\n"
               "Try 1-20 depending on your volume's density range.");
    Double_knob(f, &_scattering, "scattering", "Scattering");
    Tooltip(f, "Controls how much light scatters through the volume.\n"
               "Higher = brighter volume. 0 = completely dark.");

    Divider(f, "Lighting");
    XYZ_knob(f,   _lightDir,   "light_dir",   "Light Direction");
    Tooltip(f, "Direction the light comes from (will be normalised).");
    Color_knob(f, _lightColor, "light_color", "Light Color");
    Tooltip(f, "Color and intensity of the light source.");

    Divider(f, "Display");
    Bool_knob(f, &_showBbox, "show_bbox", "Show Bounding Box");
    Tooltip(f, "Draw a green wireframe around the volume in the output.\n"
               "Helps you find the volume and frame your camera.");
}

int VDBRenderIop::knob_changed(Knob* k)
{
    if (k->is("file") || k->is("grid_name") || k->is("frame_offset")) {
        _gridValid = false;
        return 1;
    }
    return Iop::knob_changed(k);
}

// ─── Camera helper ───────────────────────────────────────────────────────────

CameraOp* VDBRenderIop::camera() const
{
    return dynamic_cast<CameraOp*>(Op::input(0));
}

const char* VDBRenderIop::input_label(int idx, char* buf) const
{
    if (idx == 0) return "cam";
    return Iop::input_label(idx, buf);
}

bool VDBRenderIop::test_input(int idx, Op* op) const
{
    if (idx == 0) {
        // Accept CameraOp, or any default input Nuke provides
        if (dynamic_cast<CameraOp*>(op)) return true;
        return Iop::test_input(idx, op);
    }
    return Iop::test_input(idx, op);
}

// ─── Frame sequence helper ───────────────────────────────────────────────────

std::string VDBRenderIop::resolveFramePath(int frame) const
{
    std::string path(_vdbFilePath ? _vdbFilePath : "");
    if (path.empty()) return path;

    // Method 1: Replace #### patterns with frame number
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

    // Method 2: Replace %0Nd printf patterns
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

    // Method 3: Auto-detect — find the last group of digits before the
    // file extension and replace with the current frame number.
    // e.g. dust_impact_0099.vdb → finds "0099", padding=4, replaces with frame
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) dot = path.size();

    // Walk backwards from the dot to find digits
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

    // No pattern found — return as-is (single file, no sequence)
    return path;
}

// ─── append (frame sensitivity) ──────────────────────────────────────────────

void VDBRenderIop::append(Hash& hash)
{
    // Include the current frame in the hash so Nuke re-renders on frame change
    hash.append(outputContext().frame());
    hash.append(_frameOffset);
}

// ─── 3D Viewport handles ────────────────────────────────────────────────────

void VDBRenderIop::build_handles(ViewerContext* ctx)
{
    // Only draw when we have a valid grid and bbox display is on
    if (!_showBbox || !_gridValid) return;

    // Register ourselves for draw_handle callback
    add_draw_handle(ctx);
}

void VDBRenderIop::draw_handle(ViewerContext* ctx)
{
    if (!_showBbox || !_gridValid) return;

    // Build the 8 world-space corners of the bbox
    float corners[8][3];
    int ci = 0;
    for (int iz = 0; iz <= 1; ++iz)
        for (int iy = 0; iy <= 1; ++iy)
            for (int ix = 0; ix <= 1; ++ix) {
                corners[ci][0] = static_cast<float>(ix ? _bboxMax[0] : _bboxMin[0]);
                corners[ci][1] = static_cast<float>(iy ? _bboxMax[1] : _bboxMin[1]);
                corners[ci][2] = static_cast<float>(iz ? _bboxMax[2] : _bboxMin[2]);
                ++ci;
            }

    // Draw wireframe box
    glPushAttrib(GL_CURRENT_BIT | GL_LINE_BIT | GL_ENABLE_BIT);
    glDisable(GL_LIGHTING);
    glLineWidth(1.5f);
    glColor3f(0.0f, 1.0f, 0.0f); // green

    glBegin(GL_LINES);
    for (int e = 0; e < 12; ++e) {
        int i0 = _bboxEdges[e][0];
        int i1 = _bboxEdges[e][1];
        glVertex3f(corners[i0][0], corners[i0][1], corners[i0][2]);
        glVertex3f(corners[i1][0], corners[i1][1], corners[i1][2]);
    }
    glEnd();

    // Draw center cross
    float cx = static_cast<float>((_bboxMin[0] + _bboxMax[0]) * 0.5);
    float cy = static_cast<float>((_bboxMin[1] + _bboxMax[1]) * 0.5);
    float cz = static_cast<float>((_bboxMin[2] + _bboxMax[2]) * 0.5);
    float sz = static_cast<float>(
        std::max({_bboxMax[0] - _bboxMin[0],
                  _bboxMax[1] - _bboxMin[1],
                  _bboxMax[2] - _bboxMin[2]}) * 0.05);

    glColor3f(1.0f, 1.0f, 0.0f); // yellow center cross
    glBegin(GL_LINES);
    glVertex3f(cx - sz, cy, cz); glVertex3f(cx + sz, cy, cz);
    glVertex3f(cx, cy - sz, cz); glVertex3f(cx, cy + sz, cz);
    glVertex3f(cx, cy, cz - sz); glVertex3f(cx, cy, cz + sz);
    glEnd();

    glPopAttrib();
}

// ─── _validate ───────────────────────────────────────────────────────────────

void VDBRenderIop::_validate(bool for_real)
{
    _camValid = false;

    // Don't call copy_info() — input 0 may be a CameraOp, not an Iop.
    // Use the Format knob to set output resolution.
    const Format* fullFmt = _formats.fullSizeFormat();
    const Format* proxyFmt = _formats.format();
    if (!fullFmt) fullFmt = proxyFmt;
    if (fullFmt) {
        info_.format(*fullFmt);
        info_.full_size_format(*fullFmt);
        info_.set(*fullFmt);  // set the data window (bounding box) to full format
    }
    info_.channels(Mask_RGBA);
    set_out_channels(Mask_RGBA);
    info_.turn_on(Mask_RGBA);

    if (!for_real) return;

    // Cache camera — CameraOp not thread-safe, must not call from engine()
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

    // Load OpenVDB grid
    {
        Guard guard(_loadLock);

        // Resolve frame number for sequences
        int curFrame = static_cast<int>(outputContext().frame()) + _frameOffset;
        std::string path = resolveFramePath(curFrame);
        std::string grid(_gridName ? _gridName : "");

        // If resolved path doesn't exist, fall back to the original path
        // (handles single files with numbers in the name that aren't sequences)
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
                    if (origTest) {
                        fclose(origTest);
                        path = origPath;
                    }
                }
            } else {
                fclose(test);
            }
        }

        if (!_gridValid || path != _loadedPath || grid != _loadedGrid || curFrame != _loadedFrame) {
            _floatGrid.reset();
            _gridValid = false;

            if (!path.empty()) {
                try {
                    // Sanitise path
                    std::string cleanPath = path;
                    for (char& ch : cleanPath)
                        if (ch == '\\') ch = '/';

                    openvdb::io::File file(cleanPath);
                    file.open();

                    // If no grid name given, use "density" as default
                    std::string targetName = grid.empty() ? "density" : grid;

                    openvdb::GridBase::Ptr baseGrid;

                    // Iterate grids to find our target float grid
                    bool found = false;
                    for (auto it = file.beginName(); it != file.endName(); ++it) {
                        if (it.gridName() == targetName) {
                            baseGrid = file.readGrid(targetName);
                            found = true;
                            break;
                        }
                    }

                    if (!found) {
                        // If target not found, try reading the first float grid
                        for (auto it = file.beginName(); it != file.endName(); ++it) {
                            auto g = file.readGrid(it.gridName());
                            if (g->isType<openvdb::FloatGrid>()) {
                                baseGrid = g;
                                found = true;
                                break;
                            }
                        }
                    }

                    file.close();

                    if (!found || !baseGrid) {
                        error("No float grid '%s' found in %s",
                              targetName.c_str(), cleanPath.c_str());
                        return;
                    }

                    if (!baseGrid->isType<openvdb::FloatGrid>()) {
                        error("Grid '%s' is not a float grid.",
                              targetName.c_str());
                        return;
                    }

                    _floatGrid = openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);

                    // Compute world-space bounding box from active voxels
                    openvdb::CoordBBox activeBBox = _floatGrid->evalActiveVoxelBoundingBox();
                    if (activeBBox.empty()) {
                        error("Grid has no active voxels.");
                        return;
                    }

                    // Convert index-space bbox corners to world space
                    const openvdb::math::Transform& xform = _floatGrid->transform();
                    openvdb::Vec3d corners[8];
                    const openvdb::Coord& lo = activeBBox.min();
                    const openvdb::Coord& hi = activeBBox.max();
                    int ci = 0;
                    for (int iz = 0; iz <= 1; ++iz)
                        for (int iy = 0; iy <= 1; ++iy)
                            for (int ix = 0; ix <= 1; ++ix) {
                                openvdb::Vec3d ip(
                                    ix ? hi.x() + 1.0 : lo.x(),
                                    iy ? hi.y() + 1.0 : lo.y(),
                                    iz ? hi.z() + 1.0 : lo.z());
                                corners[ci++] = xform.indexToWorld(ip);
                            }

                    _bboxMin = corners[0];
                    _bboxMax = corners[0];
                    for (int i = 1; i < 8; ++i) {
                        for (int ax = 0; ax < 3; ++ax) {
                            _bboxMin[ax] = std::min(_bboxMin[ax], corners[i][ax]);
                            _bboxMax[ax] = std::max(_bboxMax[ax], corners[i][ax]);
                        }
                    }

                    _gridValid   = true;
                    _loadedPath  = path;
                    _loadedGrid  = grid;
                    _loadedFrame = curFrame;

                    // Print bbox info to Nuke console
                    std::printf("VDBRender: Loaded grid from %s\n", cleanPath.c_str());
                    std::printf("  BBox min: (%.3f, %.3f, %.3f)\n",
                        _bboxMin[0], _bboxMin[1], _bboxMin[2]);
                    std::printf("  BBox max: (%.3f, %.3f, %.3f)\n",
                        _bboxMax[0], _bboxMax[1], _bboxMax[2]);
                    std::printf("  Center:   (%.3f, %.3f, %.3f)\n",
                        (_bboxMin[0]+_bboxMax[0])*0.5,
                        (_bboxMin[1]+_bboxMax[1])*0.5,
                        (_bboxMin[2]+_bboxMax[2])*0.5);
                    double diag = std::sqrt(
                        std::pow(_bboxMax[0]-_bboxMin[0], 2) +
                        std::pow(_bboxMax[1]-_bboxMin[1], 2) +
                        std::pow(_bboxMax[2]-_bboxMin[2], 2));
                    std::printf("  Size:     (%.3f, %.3f, %.3f)  Diagonal: %.3f\n",
                        _bboxMax[0]-_bboxMin[0],
                        _bboxMax[1]-_bboxMin[1],
                        _bboxMax[2]-_bboxMin[2], diag);
                    std::printf("  Tip: Point camera at center, pull back ~%.0f units\n",
                        diag * 2.0);
                }
                catch (const openvdb::Exception& e) {
                    error("OpenVDB: %s", e.what());
                }
                catch (const std::exception& e) {
                    error("OpenVDB: %s", e.what());
                }
                catch (...) {
                    error("OpenVDB: unknown exception loading file.");
                }
            }
        }
    }

    // Project bbox corners to screen space for wireframe overlay
    if (_gridValid && _camValid && _showBbox) {
        const Format& fmt = format();
        const int W = fmt.width();
        const int H = fmt.height();
        const double halfH = _halfW * static_cast<double>(H) / static_cast<double>(W);

        // Build the 8 world-space corners of the bbox
        openvdb::Vec3d wCorners[8];
        int ci = 0;
        for (int iz = 0; iz <= 1; ++iz)
            for (int iy = 0; iy <= 1; ++iy)
                for (int ix = 0; ix <= 1; ++ix)
                    wCorners[ci++] = openvdb::Vec3d(
                        ix ? _bboxMax[0] : _bboxMin[0],
                        iy ? _bboxMax[1] : _bboxMin[1],
                        iz ? _bboxMax[2] : _bboxMin[2]);

        // Project each corner: world → camera-local → screen
        for (int i = 0; i < 8; ++i) {
            // Camera-local position
            double dx = wCorners[i][0] - _camOrigin[0];
            double dy = wCorners[i][1] - _camOrigin[1];
            double dz = wCorners[i][2] - _camOrigin[2];
            // Transform by inverse camera rotation (transpose since orthonormal)
            double cx = _camRot[0][0]*dx + _camRot[0][1]*dy + _camRot[0][2]*dz;
            double cy = _camRot[1][0]*dx + _camRot[1][1]*dy + _camRot[1][2]*dz;
            double cz = _camRot[2][0]*dx + _camRot[2][1]*dy + _camRot[2][2]*dz;
            // Behind camera?
            if (cz >= -1e-6) {
                _screenCorners[i] = {0, 0, false};
                continue;
            }
            // Perspective divide
            double ndcX = (cx / -cz) / _halfW;
            double ndcY = (cy / -cz) / halfH;
            // NDC [-1,1] → pixel coords
            double sx = (ndcX * 0.5 + 0.5) * W;
            double sy = (ndcY * 0.5 + 0.5) * H;
            _screenCorners[i] = {sx, sy, true};
        }
    }
}

// ─── _request ────────────────────────────────────────────────────────────────

void VDBRenderIop::_request(int x, int y, int r, int t,
                             ChannelMask channels, int count)
{
}

// ─── engine() ────────────────────────────────────────────────────────────────

void VDBRenderIop::engine(int y, int x, int r,
                           ChannelMask channels, Row& row)
{
    if (!_gridValid || !_floatGrid || !_camValid) {
        foreach (z, channels) {
            float* ptr = row.writable(z);
            for (int ix = x; ix < r; ++ix)
                ptr[ix] = 0.0f;
        }
        return;
    }

    const Format& fmt = format();
    const int     W   = fmt.width();
    const int     H   = fmt.height();
    const double halfW = _halfW;
    const double halfH = _halfW * static_cast<double>(H) /
                                  static_cast<double>(W);

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

        openvdb::Vec3d rayDir(
            _camRot[0][0]*rcx + _camRot[1][0]*rcy + _camRot[2][0]*rcz,
            _camRot[0][1]*rcx + _camRot[1][1]*rcy + _camRot[2][1]*rcz,
            _camRot[0][2]*rcx + _camRot[1][2]*rcy + _camRot[2][2]*rcz
        );

        const double len = std::sqrt(rayDir[0]*rayDir[0] +
                                     rayDir[1]*rayDir[1] +
                                     rayDir[2]*rayDir[2]);
        if (len > 1e-8) rayDir *= 1.0 / len;

        float R = 0, G = 0, B = 0, A = 0;
        marchRay(_camOrigin, rayDir, R, G, B, A);

        rOut[ix] = R;
        gOut[ix] = G;
        bOut[ix] = B;
        aOut[ix] = A;
    }

    // Draw wireframe bounding box overlay
    if (_showBbox && _gridValid && _camValid) {
        const double lineWidth = 1.5;
        const double scanY = static_cast<double>(y);

        for (int e = 0; e < 12; ++e) {
            const ScreenPt& p0 = _screenCorners[_bboxEdges[e][0]];
            const ScreenPt& p1 = _screenCorners[_bboxEdges[e][1]];
            if (!p0.valid || !p1.valid) continue;

            // Check if this edge's Y range covers the current scanline
            double yMin = std::min(p0.y, p1.y) - lineWidth;
            double yMax = std::max(p0.y, p1.y) + lineWidth;
            if (scanY < yMin || scanY > yMax) continue;

            // Find X at this scanline by lerping along the edge
            double dy = p1.y - p0.y;
            if (std::abs(dy) < 0.001) {
                // Nearly horizontal edge — draw full span
                int x0 = std::max(x, static_cast<int>(std::min(p0.x, p1.x) - lineWidth));
                int x1 = std::min(r, static_cast<int>(std::max(p0.x, p1.x) + lineWidth + 1));
                for (int ix = x0; ix < x1; ++ix) {
                    if (std::abs(scanY - p0.y) <= lineWidth) {
                        rOut[ix] = 0.0f; gOut[ix] = 1.0f;
                        bOut[ix] = 0.0f; aOut[ix] = 1.0f;
                    }
                }
            } else {
                double t = (scanY - p0.y) / dy;
                if (t < -0.01 || t > 1.01) continue;
                double hitX = p0.x + t * (p1.x - p0.x);
                int ix0 = std::max(x, static_cast<int>(hitX - lineWidth));
                int ix1 = std::min(r, static_cast<int>(hitX + lineWidth + 1));
                for (int ix = ix0; ix < ix1; ++ix) {
                    rOut[ix] = 0.0f; gOut[ix] = 1.0f;
                    bOut[ix] = 0.0f; aOut[ix] = 1.0f;
                }
            }
        }
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
        double invD = (std::abs(dir[axis]) > 1e-8)
                      ? 1.0 / dir[axis] : 1e38;
        double t0 = (_bboxMin[axis] - origin[axis]) * invD;
        double t1 = (_bboxMax[axis] - origin[axis]) * invD;
        if (t0 > t1) std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit  = std::min(tExit,  t1);
    }
    if (tEnter >= tExit || tExit <= 0.0) return;

    // Per-call accessor — each thread gets its own, so this is safe
    openvdb::FloatGrid::ConstAccessor acc = _floatGrid->getConstAccessor();
    const openvdb::math::Transform&  xform = _floatGrid->transform();

    // Light direction
    openvdb::Vec3d lDir(_lightDir[0], _lightDir[1], _lightDir[2]);
    {
        double lLen = std::sqrt(lDir[0]*lDir[0] +
                                lDir[1]*lDir[1] +
                                lDir[2]*lDir[2]);
        if (lLen > 1e-8) lDir *= 1.0 / lLen;
    }

    const double step       = _stepSize;
    const double extinction = _extinction;
    const double scattering = _scattering;
    double transmittance    = 1.0;
    double accR = 0.0, accG = 0.0, accB = 0.0;

    double t = tEnter;
    while (t < tExit && transmittance > 0.001)
    {
        openvdb::Vec3d wPos(origin[0] + t * dir[0],
                            origin[1] + t * dir[1],
                            origin[2] + t * dir[2]);

        // World → index space via the grid's transform
        openvdb::Vec3d iPos = xform.worldToIndex(wPos);
        openvdb::Coord ijk(
            static_cast<int>(std::floor(iPos[0])),
            static_cast<int>(std::floor(iPos[1])),
            static_cast<int>(std::floor(iPos[2])));
        float density = acc.getValue(ijk);

        if (density > 1e-6f)
        {
            // Shadow ray toward light (coarse, 16 steps)
            double lightT = 1.0;
            {
                openvdb::FloatGrid::ConstAccessor lAcc =
                    _floatGrid->getConstAccessor();
                const double lightStep = step * 4.0;
                double lt = 0.0;
                for (int ls = 0; ls < 16; ++ls) {
                    openvdb::Vec3d lWPos(wPos[0] + lt * lDir[0],
                                         wPos[1] + lt * lDir[1],
                                         wPos[2] + lt * lDir[2]);
                    bool inside = true;
                    for (int ax = 0; ax < 3; ++ax)
                        if (lWPos[ax] < _bboxMin[ax] ||
                            lWPos[ax] > _bboxMax[ax])
                        { inside = false; break; }
                    if (!inside) break;
                    openvdb::Vec3d lIPos = xform.worldToIndex(lWPos);
                    openvdb::Coord lijk(
                        static_cast<int>(std::floor(lIPos[0])),
                        static_cast<int>(std::floor(lIPos[1])),
                        static_cast<int>(std::floor(lIPos[2])));
                    float lDensity = lAcc.getValue(lijk);
                    lightT *= std::exp(-lDensity * extinction * lightStep);
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
