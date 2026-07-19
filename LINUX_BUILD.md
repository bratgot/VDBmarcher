# VDBRender — Linux build

Linux port of [VDBRender](https://github.com/bratgot/VDBmarcher) by
**Marten Blumen** ([@bratgot](https://github.com/bratgot)). This branch adds a
Linux build system; the plugin source is unmodified upstream code.

Port by [Soup Kitchen Films](https://github.com/soupkitchenfilms), developed
with the assistance of Claude (Anthropic).

**Verified**: AlmaLinux 9.8, Nuke 17.0v3, upstream v3.1 — builds clean and
the node loads with all knobs (`nuke -t` smoke test). Originally ported and
in production since Nuke 17.0v1 on Rocky Linux 9.

> **Note from the maintainer:** Linux support is community-contributed and
> community-maintained. I develop and test on Windows only, so I
> can't personally verify Linux builds or debug Linux-specific issues.
> Please file issues, but expect fixes to come from the community rather
> than from me directly.

---

## Quick start

```bash
sudo dnf install -y cmake gcc-c++ git imath-devel   # RHEL 9 family
./build_linux.sh
```

The script:

1. Fetches **headers only** for OpenVDB 12.0.0 and oneTBB 2021.13 into
   `.deps/` (first run; shallow clones).
2. Installs a pre-generated `openvdb/version.h` (see below).
3. Builds `VDBRender.so` for every Nuke 17+ found under `/usr/local/Nuke*`,
   linking against **Nuke's own bundled** `libopenvdb.so` and `libtbb.so`.
4. Installs to `~/.nuke/plugins/VDBRender/nuke<major>/`
   (override with `DEST=/path ./build_linux.sh`).
5. Prints menu-registration instructions (`INSTALL_MENU=1` appends the
   loader block from `VDBRender_menu.py` to `~/.nuke/menu.py` for you).

No vcpkg, no second OpenVDB build — the heavy dependencies are the ones
Nuke already ships.

---

## Why it's built this way

| Decision | Reason |
|---|---|
| **C++17, not C++20** | Nuke 17's `GeoInfo.h` (`RefCountedPtr`) fails aggregate initialisation under C++20. The NDK specifies C++17. |
| **Link Nuke's `libopenvdb.so`** | Avoids loading two OpenVDB copies into one process, and matches the ABI Nuke itself uses (OpenVDB 12.0, ABI 12). |
| **oneTBB 2021.13 headers, not system `tbb-devel`** | EL9's `tbb-devel` is TBB 2020.3; its `tbb::task` API was removed in oneTBB. Compiling against it links fine but dies at runtime with `undefined symbol: _ZTIN3tbb4taskE`. Headers must match Nuke's bundled `libtbb.so.12`. |
| **Pre-generated `openvdb/version.h`** (`linux/openvdb_version.h`) | OpenVDB generates this at its own CMake configure time. For a header-only consumer we ship it with `OPENVDB_USE_EXPLICIT_INSTANTIATION` disabled — enabled, it emits `extern template` references to symbols only present in a full OpenVDB build. |
| **`-mavx2 -mfma -ffast-math`** | Matches the upstream Windows build flags. |
| **Nuke 15/16 skipped** | They bundle pre-oneTBB TBB; the oneTBB headers won't match. The script detects this (`libtbb.so.12`) and skips automatically. |

## Layout

```
linux/CMakeLists.txt      # Linux build (self-contained; upstream files untouched)
linux/openvdb_version.h   # pre-generated version.h for header-only OpenVDB use
build_linux.sh            # driver: fetch headers, build all Nukes, install
```

## Troubleshooting

- **`undefined symbol: _ZTIN3tbb4taskE`** at load — you compiled against old
  TBB headers. Make sure `ONETBB_HEADER_DIR` points at oneTBB 2021.x.
- **`Cannot find source file: make_cd_test.cpp`** — only affects the upstream
  root `CMakeLists.txt` (Windows); the Linux build in `linux/` doesn't build
  that target.
- **Different Nuke version** — check what it bundles
  (`ls /usr/local/Nuke*/libopenvdb.so* libtbb.so*`) and match the header tags
  at the top of `build_linux.sh`.
