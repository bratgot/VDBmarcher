"""
make_cd_test.py — Generate a VDB with density + Cd colour grid for testing
VDBRender's per-voxel scatter albedo (Cd attribute).

Creates a 64^3 sphere with:
  - density grid:  Gaussian falloff from sphere surface inward
  - Cd (Vec3s) grid: smooth RGB colour gradient across the volume

Usage:
  python make_cd_test.py [output_path]

Requires:
  pip install openvdb   (https://pypi.org/project/openvdb/)

If openvdb Python is not available, falls back to writing a Houdini
Python SOP snippet that can be pasted into a Python SOP.

Output: cd_test.vdb (or the path you specify)
"""

import sys
import math

OUTPUT_PATH = sys.argv[1] if len(sys.argv) > 1 else "cd_test.vdb"

try:
    import openvdb as vdb
    HAS_OPENVDB = True
except ImportError:
    HAS_OPENVDB = False

def make_vdb_python():
    """Generate using openvdb Python bindings."""
    RES = 64
    VOXEL_SIZE = 1.0 / RES

    density_grid = vdb.FloatGrid()
    density_grid.name = "density"
    density_grid.transform = vdb.createLinearTransform(voxelSize=VOXEL_SIZE)

    cd_grid = vdb.Vec3SGrid()
    cd_grid.name = "Cd"
    cd_grid.transform = vdb.createLinearTransform(voxelSize=VOXEL_SIZE)

    d_acc  = density_grid.getAccessor()
    cd_acc = cd_grid.getAccessor()

    cx = cy = cz = RES // 2
    radius = RES * 0.4

    for iz in range(RES):
        for iy in range(RES):
            for ix in range(RES):
                dx = ix - cx
                dy = iy - cy
                dz = iz - cz
                dist = math.sqrt(dx*dx + dy*dy + dz*dz)

                if dist > radius:
                    continue

                # Density: Gaussian inside sphere, zero outside
                t = 1.0 - dist / radius   # 0 at edge, 1 at centre
                density = math.exp(-3.0 * (1.0 - t)**2)

                if density < 0.001:
                    continue

                d_acc.setValueOn((ix, iy, iz), density)

                # Cd: RGB colour gradient
                #   R = gradient along X (left=0, right=1)
                #   G = gradient along Y (bottom=0, top=1)
                #   B = gradient along Z (front=0, back=1)
                # Normalised to sphere extent so colours are vivid at the edge
                r = max(0.0, min(1.0, 0.5 + dx / (radius * 2.0)))
                g = max(0.0, min(1.0, 0.5 + dy / (radius * 2.0)))
                b = max(0.0, min(1.0, 0.5 + dz / (radius * 2.0)))
                cd_acc.setValueOn((ix, iy, iz), (r, g, b))

    vdb.write(OUTPUT_PATH, grids=[density_grid, cd_grid])
    print(f"Wrote {OUTPUT_PATH}")
    print(f"  density grid: {density_grid.activeVoxelCount()} active voxels")
    print(f"  Cd grid:      {cd_grid.activeVoxelCount()} active voxels")
    print()
    print("In VDBRender:")
    print("  1. Set VDB File to this .vdb")
    print("  2. Click Discover Grids")
    print("  3. Set Render Mode to Lit")
    print("  4. Set Color Grid to 'Cd'")
    print("  5. The scatter colour should show RGB gradients across the sphere")


def make_houdini_snippet():
    """Print a Houdini Python SOP snippet as fallback."""
    snippet = '''
# Houdini Python SOP — paste into a Python SOP node
# Creates a volume with density + Cd colour grid, then export with
# File SOP (set to .vdb) or ROP Output Driver

import hou
import math

node = hou.pwd()
geo  = node.geometry()

# ── Parameters ──
RES    = 64
VSIZE  = 1.0 / RES
RADIUS = RES * 0.4
CX = CY = CZ = RES // 2

# Create density volume
den_vol = geo.createVolume(RES, RES, RES)
den_vol.setName("density")

# Create Cd volume (stored as three float volumes: Cd.r, Cd.g, Cd.b)
cd_r = geo.createVolume(RES, RES, RES)
cd_g = geo.createVolume(RES, RES, RES)
cd_b = geo.createVolume(RES, RES, RES)
cd_r.setName("Cd.r"); cd_g.setName("Cd.g"); cd_b.setName("Cd.b")

for iz in range(RES):
    for iy in range(RES):
        for ix in range(RES):
            dx = ix - CX; dy = iy - CY; dz = iz - CZ
            dist = math.sqrt(dx*dx + dy*dy + dz*dz)
            if dist > RADIUS:
                continue
            t = 1.0 - dist / RADIUS
            density = math.exp(-3.0 * (1.0 - t)**2)
            if density < 0.001:
                continue
            den_vol.setVoxel(hou.Vector3(ix, iy, iz), density)
            r = max(0.0, min(1.0, 0.5 + dx / (RADIUS * 2.0)))
            g = max(0.0, min(1.0, 0.5 + dy / (RADIUS * 2.0)))
            b = max(0.0, min(1.0, 0.5 + dz / (RADIUS * 2.0)))
            cd_r.setVoxel(hou.Vector3(ix, iy, iz), r)
            cd_g.setVoxel(hou.Vector3(ix, iy, iz), g)
            cd_b.setVoxel(hou.Vector3(ix, iy, iz), b)

print("Done — export with File SOP to .vdb")
'''
    print("openvdb Python not found. Use this Houdini Python SOP snippet instead:")
    print(snippet)
    print()
    print("Or install openvdb Python:")
    print("  pip install openvdb")


if HAS_OPENVDB:
    make_vdb_python()
else:
    make_houdini_snippet()
