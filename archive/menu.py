import nuke

# VDBRender — OpenVDB ray marcher
# Reads .vdb files directly. No NanoVDB conversion needed.
# Supports any float density grid (default grid name: "density").

# If VDBRender.dll is not in Nuke's default plugin path, add it:
# nuke.pluginAddPath("C:/path/to/your/plugin/folder")

nuke.load("VDBRender")

toolbar = nuke.menu("Nodes")
vdb_menu = toolbar.addMenu("VDB")
vdb_menu.addCommand("VDBRender", "nuke.createNode('VDBRender')")
