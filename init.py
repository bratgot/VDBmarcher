# VDBRender init.py — runs BEFORE Nuke scans for plugin DLLs
# Adds lib/ subfolder to DLL search path so Windows can find
# openvdb.dll, tbb12.dll, etc.

import os
import nuke

_plugin_dir = os.path.dirname(__file__)
_lib_dir = os.path.join(_plugin_dir, "lib")

if os.path.isdir(_lib_dir):
    os.environ["PATH"] = _lib_dir + os.pathsep + os.environ.get("PATH", "")
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(_lib_dir)

nuke.pluginAddPath(_plugin_dir)
