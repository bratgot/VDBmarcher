# Add to your ~/.nuke/init.py or merge with existing:
# Ensures LibTorch DLLs are loadable before VDBRender.dll loads

import os
import sys

# LibTorch runtime — adjust path if installed elsewhere
libtorch_lib = r"C:\libtorch\lib"
if os.path.isdir(libtorch_lib):
    os.environ["PATH"] = libtorch_lib + os.pathsep + os.environ.get("PATH", "")
    if hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(libtorch_lib)
