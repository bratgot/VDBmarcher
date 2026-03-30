// make_cd_test.cpp — Generates cd_test.vdb with density + Cd colour grid
// Build: included as a CMake target (see CMakeLists.txt addition below)
//
// Output: cd_test.vdb (path from argv[1], default ./cd_test.vdb)
//   density (FloatGrid)  — Gaussian sphere, 64^3 voxels
//   Cd      (Vec3SGrid)  — RGB gradient: R=X, G=Y, B=Z axis

#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <cmath>
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    openvdb::initialize();

    const std::string outPath = (argc > 1) ? argv[1] : "cd_test.vdb";

    const int    RES    = 64;
    const double VSIZE  = 2.0 / RES;   // world fits in [-1,+1]^3
    const double RADIUS = 1.0;

    // ── Density grid (FloatGrid) ──────────────────────────────────────────
    auto densGrid = openvdb::FloatGrid::create(0.0f);
    densGrid->setName("density");
    densGrid->setTransform(openvdb::math::Transform::createLinearTransform(VSIZE));

    auto dAcc = densGrid->getAccessor();

    // ── Cd colour grid (Vec3SGrid) ────────────────────────────────────────
    auto cdGrid = openvdb::Vec3SGrid::create(openvdb::Vec3s(0,0,0));
    cdGrid->setName("Cd");
    cdGrid->setTransform(openvdb::math::Transform::createLinearTransform(VSIZE));

    auto cAcc = cdGrid->getAccessor();

    int written = 0;
    for (int iz = 0; iz < RES; ++iz)
    for (int iy = 0; iy < RES; ++iy)
    for (int ix = 0; ix < RES; ++ix) {
        // World position [-1, +1]
        const double wx = -1.0 + (ix + 0.5) * VSIZE;
        const double wy = -1.0 + (iy + 0.5) * VSIZE;
        const double wz = -1.0 + (iz + 0.5) * VSIZE;
        const double dist = std::sqrt(wx*wx + wy*wy + wz*wz);

        if (dist > RADIUS) continue;

        const double t = 1.0 - dist / RADIUS;                     // 0=edge, 1=centre
        const float  d = (float)std::exp(-3.0 * (1.0 - t) * (1.0 - t));  // Gaussian

        if (d < 0.001f) continue;

        const openvdb::Coord ijk(ix, iy, iz);
        dAcc.setValue(ijk, d);

        // Cd: smooth RGB gradient across each axis
        const float r = (float)std::max(0.0, std::min(1.0, 0.5 + wx / (RADIUS * 2.0)));
        const float g = (float)std::max(0.0, std::min(1.0, 0.5 + wy / (RADIUS * 2.0)));
        const float b = (float)std::max(0.0, std::min(1.0, 0.5 + wz / (RADIUS * 2.0)));
        cAcc.setValue(ijk, openvdb::Vec3s(r, g, b));

        ++written;
    }

    openvdb::io::File file(outPath);
    file.write({densGrid, cdGrid});
    file.close();

    std::printf("Wrote %s\n", outPath.c_str());
    std::printf("  %d active voxels\n", written);
    std::printf("  density grid: Gaussian sphere, 64^3\n");
    std::printf("  Cd grid:      R=X, G=Y, B=Z gradient\n\n");
    std::printf("In VDBRender:\n");
    std::printf("  1. Set VDB File to this path\n");
    std::printf("  2. Click Discover Grids\n");
    std::printf("  3. Set Render Mode to Lit\n");
    std::printf("  4. Cd is auto-detected — the sphere should\n");
    std::printf("     show RGB colour variation under lighting\n");

    return 0;
}
