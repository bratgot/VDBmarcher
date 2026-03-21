#include "VDBRenderIop.h"
#include <DDImage/Knob.h>
#include <DDImage/Format.h>
#include <cmath>
#include <algorithm>

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
    File_knob(f, &_vdbFilePath, "file", "VDB File");
    String_knob(f, &_gridName, "grid_name", "Grid Name");
    Divider(f, "Ray March");
    Double_knob(f, &_stepSize,   "step_size",  "Step Size");
    Double_knob(f, &_extinction, "extinction", "Extinction");
    Double_knob(f, &_scattering, "scattering", "Scattering");
    Divider(f, "Lighting");
    XYZ_knob(f,   _lightDir,   "light_dir",   "Light Direction");
    Color_knob(f, _lightColor, "light_color", "Light Color");
}

int VDBRenderIop::knob_changed(Knob* k)
{
    if (k->is("file") || k->is("grid_name")) {
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

// ─── _validate ───────────────────────────────────────────────────────────────

void VDBRenderIop::_validate(bool for_real)
{
    _camValid  = false;
    _gridValid = false;
    copy_info();
    set_out_channels(Mask_RGBA);
    info_.turn_on(Mask_RGBA);

    if (!for_real) return;

    // Cache camera — CameraOp not thread-safe, must not call from engine()
    if (CameraOp* cam = camera()) {
        const Matrix4 cw = cam->matrix();
        _camOrigin = openvdb::Vec3d(cw[3][0], cw[3][1], cw[3][2]);
        for (int c = 0; c < 3; ++c)
            for (int r = 0; r < 3; ++r)
                _camRot[c][r] = static_cast<double>(cw[c][r]);
        const double fl = static_cast<double>(cam->focal_length());
        const double fw = static_cast<double>(cam->film_width());
        _halfW    = (fw * 0.5) / fl;
        _camValid = true;
    }

    // Load OpenVDB grid
    {
        Guard guard(_loadLock);
        std::string path(_vdbFilePath ? _vdbFilePath : "");
        std::string grid(_gridName   ? _gridName   : "");

        if (!_gridValid || path != _loadedPath || grid != _loadedGrid) {
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

                    _gridValid  = true;
                    _loadedPath = path;
                    _loadedGrid = grid;
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
