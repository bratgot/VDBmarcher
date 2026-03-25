VDBRender — OpenVDB Volume Ray Marcher

Render OpenVDB volumes directly inside Nuke. No external renderer, no round-tripping. Load a .vdb, connect a camera, pick a preset — done.

VDBRender is a single C++ NDK node that ray marches density, temperature, flame, velocity and colour grids with physically-based lighting, deep output, emission, motion blur and AOV passes. It ships with built-in sun/sky and studio lighting so you get a usable render the moment you load a file.


WHAT YOU GET

— Physically-based volume rendering with Beer-Lambert transmittance and Henyey-Greenstein phase function
— HDDA empty-space skipping for fast sparse volumes
— Emission-driven in-scatter: fire illuminates surrounding smoke
— Multiple scattering (1-4 bounces)
— Analytical sun/sky model (Preetham) with 6 presets: Day Sky, Golden Hour, Overcast, Blue Hour, Night, Off
— Studio lighting with 6 presets: 3-Point, Dramatic, Soft, Rim Only, Top Light, Off
— Independent Sky Mix and Studio Mix sliders — blend outdoor and studio light freely
— Environment map support from Nuke's Environment node
— Deep output for compositing volumes with CG geometry
— Separated AOV passes: Density, Emission, Shadow, Depth
— Velocity-based motion blur
— 9 scene presets: Thin Smoke, Dense Smoke, Fog/Mist, Cumulus Cloud, Fire, Explosion, Pyroclastic, Dust Storm, Steam
— Discover Grids button auto-detects all grids in the file
— Auto Sequence converts numbered filenames to #### padding
— 3D viewport bounding box and point cloud preview
— AVX2/FMA compiled, per-scanline accessor pooling, AABB early-out


RENDER MODES

Lit — full physically-based scatter with shadow rays
Greyscale — density to luminance
Heat — black to red to yellow to white
Cool — black to blue to cyan to white
Blackbody — Planckian locus temperature mapping
Custom Gradient — user two-colour ramp
Explosion — lit smoke with self-luminous fire and emission in-scatter


GRIDS

Density (float): density, smoke, soot
Temperature (float): temperature, heat, temp
Flames (float): flame, fire, fuel, burn
Velocity (vec3): vel, v, velocity
Colour (vec3): Cd, color, colour, rgb, albedo


COMPATIBILITY

Nuke 17 only. Nuke 14-16 ship an incompatible TBB version. Windows. Linux/Mac not yet tested.


INSTALLATION

Option A — Automatic (recommended)

1. Download VDBRender-2.1-nuke17-win.zip from the GitHub releases page
2. Extract anywhere
3. Double-click install.bat
4. Restart Nuke
5. VDBRender appears under VDB > VDBRender in the toolbar

The installer copies all plugin files to ~/.nuke/plugins/VDBRender/nuke17/ and appends the menu loader to your menu.py. It checks for duplicates so it is safe to run more than once.


Option B — Manual

1. Download and extract the zip

2. Copy the nuke17/ folder so you have:

    ~/.nuke/plugins/VDBRender/nuke17/VDBRender.dll
    ~/.nuke/plugins/VDBRender/nuke17/openvdb.dll
    ~/.nuke/plugins/VDBRender/nuke17/tbb12.dll
    ~/.nuke/plugins/VDBRender/nuke17/tbbmalloc.dll
    ~/.nuke/plugins/VDBRender/nuke17/Imath-3_2.dll
    ~/.nuke/plugins/VDBRender/nuke17/blosc.dll
    ~/.nuke/plugins/VDBRender/nuke17/zlib1.dll
    ~/.nuke/plugins/VDBRender/nuke17/zstd.dll
    ~/.nuke/plugins/VDBRender/nuke17/lz4.dll

3. Open (or create) ~/.nuke/menu.py and append the following at the END. Do not replace your existing menu.py:

# ── VDBRender ──
def _load_vdbrender():
    import nuke, os
    base = os.path.join(os.path.expanduser("~/.nuke"), "plugins")
    v = os.path.join(base, "VDBRender", "nuke%d" % nuke.NUKE_VERSION_MAJOR)
    if os.path.isdir(v):
        nuke.pluginAddPath(v)
        os.environ["PATH"] = v + os.pathsep + os.environ.get("PATH", "")
    try:
        nuke.load("VDBRender")
        toolbar = nuke.menu("Nodes")
        m = toolbar.addMenu("VDB", icon="")
        m.addCommand("VDBRender", "nuke.createNode('VDBRender')")
        nuke.tprint("VDBRender loaded (Nuke %d)" % nuke.NUKE_VERSION_MAJOR)
    except RuntimeError:
        nuke.tprint("VDBRender: not found for Nuke %d" % nuke.NUKE_VERSION_MAJOR)
_load_vdbrender()
# ── end VDBRender ──

4. Restart Nuke.


Uninstall

Double-click uninstall.bat from the original zip, or manually delete ~/.nuke/plugins/VDBRender/. The menu.py loader handles missing files gracefully — it prints a harmless "not found" message — so removing the menu.py block is optional.


QUICK START

1. Tab-create VDBRender
2. Set the VDB File path
3. Connect a Camera to the cam input
4. You already have Day Sky lighting and Preview quality — render away
5. Try a Scene Preset (Explosion, Cloud, Fog) to configure shading and lighting in one click
6. Adjust Lighting tab: Sky Mix and Studio Mix let you blend sun/sky with studio key/fill/rim
7. For fire: click Discover Grids to find temperature and flame grids, then pick the Fire or Explosion preset


NODE GRAPH

Single volume:
    Camera ── VDBRender ── Viewer

Over a plate with lights:
    Read (plate) ── bg
    Camera ── cam ── VDBRender ── Viewer
    Scene ── scn

Deep multi-volume:
    Camera ──┬── VDBRender (smoke) ──┐
             ├── VDBRender (fire)  ──┼── DeepMerge ── DeepToImage
             └── VDBRender (dust)  ──┘


BUILD FROM SOURCE

Requires Nuke 17 NDK, Visual Studio 2022 (clang-cl), CMake, OpenVDB 12 via vcpkg.

    vcpkg install openvdb:x64-windows
    cd C:\dev\VDBmarcher
    build_all.bat

The build script auto-detects Nuke installations and installs everything. Run make_dist.bat afterwards to assemble the distribution zip.


SOURCE CODE

https://github.com/bratgot/VDBmarcher

Apache 2.0 license. Contributions and bug reports welcome.


CREDITS

Created by Marten Blumen.
OpenVDB by the Academy Software Foundation.
Sun/sky model based on Preetham, Shirley, Smits (1999).
