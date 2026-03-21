import nuke, os

# VDBRender — OpenVDB ray marcher
# Reads .vdb files directly. Supports sequences.

plugin_dir = os.path.expanduser("~/.nuke/plugins")
os.environ["PATH"] = plugin_dir + os.pathsep + os.environ["PATH"]
nuke.pluginAddPath(plugin_dir)
nuke.load("VDBRender")

toolbar = nuke.menu("Nodes")
vdb_menu = toolbar.addMenu("VDB")
vdb_menu.addCommand("VDBRender", "nuke.createNode('VDBRender')")
