# VDBRender menu.py — Nuke menu entries
# Loaded automatically when nuke17/ is on Nuke's plugin path.
# DLL search paths are set up by init.py.

import nuke

toolbar = nuke.menu("Nodes")
vdb_menu = toolbar.addMenu("VDB", icon="")
vdb_menu.addCommand("VDBRender", "nuke.createNode('VDBRender')")
