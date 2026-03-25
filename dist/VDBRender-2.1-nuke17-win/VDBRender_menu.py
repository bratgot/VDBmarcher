# ── VDBRender ────────────────────────────────────────────────
# Auto-loads the correct VDBRender.dll for the running Nuke version.
# Append this block to your existing ~/.nuke/menu.py
# or place VDBRender_menu.py alongside it and import from menu.py.

def _load_vdbrender():
    import nuke, os
    plugin_base = os.path.join(os.path.expanduser("~/.nuke"), "plugins")
    major = nuke.NUKE_VERSION_MAJOR

    # Try version-specific: plugins/VDBRender/nuke17/
    versioned = os.path.join(plugin_base, "VDBRender", "nuke%d" % major)
    if os.path.isdir(versioned):
        nuke.pluginAddPath(versioned)
        os.environ["PATH"] = versioned + os.pathsep + os.environ.get("PATH", "")
    else:
        # Flat fallback: plugins/
        nuke.pluginAddPath(plugin_base)
        os.environ["PATH"] = plugin_base + os.pathsep + os.environ.get("PATH", "")

    try:
        nuke.load("VDBRender")
        toolbar = nuke.menu("Nodes")
        vdb_menu = toolbar.addMenu("VDB", icon="")
        vdb_menu.addCommand("VDBRender", "nuke.createNode('VDBRender')")
        nuke.tprint("VDBRender loaded (Nuke %d)" % major)
    except RuntimeError:
        nuke.tprint("VDBRender: plugin not found for Nuke %d" % major)

_load_vdbrender()
# ── end VDBRender ────────────────────────────────────────────
