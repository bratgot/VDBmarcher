# nvdb_encoder_panel.py — Nuke panel for background VDB → NVDB encoding
# Launches nvdb_encode.exe as a subprocess so Nuke stays responsive.
#
# Install:
#   1. Copy to ~/.nuke/ or your plugin path
#   2. Add to menu.py:
#        import nvdb_encoder_panel
#        nvdb_encoder_panel.register()
#
# Requires nvdb_encode.exe on PATH or set the path in the panel.

import nuke
import nukescripts
import os
import subprocess
import threading
import re
import time


class NVDBEncoderPanel(nukescripts.PythonPanel):
    """Background VDB → NVDB neural compression panel."""

    def __init__(self):
        nukescripts.PythonPanel.__init__(self, "NeuralVDB Encoder", "com.vdbrender.nvdb_encoder")

        # ── File paths ──
        self.inputKnob = nuke.File_Knob("input_file", "Input VDB")
        self.addKnob(self.inputKnob)

        self.outputKnob = nuke.File_Knob("output_file", "Output NVDB")
        self.addKnob(self.outputKnob)

        self.addKnob(nuke.Text_Knob("div1", "", ""))

        # ── Auto-fill from VDBRender node ──
        self.fromNodeKnob = nuke.PyScript_Knob("from_node", "Fill from VDBRender")
        self.addKnob(self.fromNodeKnob)

        self.addKnob(nuke.Text_Knob("div2", "", ""))

        # ── Frame range ──
        self.seqKnob = nuke.Boolean_Knob("sequence", "Sequence")
        self.seqKnob.setFlag(nuke.STARTLINE)
        self.addKnob(self.seqKnob)

        self.startKnob = nuke.Int_Knob("start_frame", "Start")
        self.startKnob.setValue(int(nuke.root().firstFrame()))
        self.addKnob(self.startKnob)

        self.endKnob = nuke.Int_Knob("end_frame", "End")
        self.endKnob.setValue(int(nuke.root().lastFrame()))
        self.addKnob(self.endKnob)

        self.warmStartKnob = nuke.Boolean_Knob("warm_start", "Warm Start")
        self.warmStartKnob.setValue(True)
        self.warmStartKnob.setTooltip(
            "Initialize each frame from the previous frame's weights.\n"
            "Faster convergence and better temporal coherency.")
        self.addKnob(self.warmStartKnob)

        self.addKnob(nuke.Text_Knob("div3", "", ""))

        # ── Training settings ──
        self.topoEpochsKnob = nuke.Int_Knob("topo_epochs", "Topo Epochs")
        self.topoEpochsKnob.setValue(50)
        self.addKnob(self.topoEpochsKnob)

        self.valEpochsKnob = nuke.Int_Knob("val_epochs", "Value Epochs")
        self.valEpochsKnob.setValue(200)
        self.addKnob(self.valEpochsKnob)

        self.valHiddenKnob = nuke.Int_Knob("val_hidden", "Value Hidden Dim")
        self.valHiddenKnob.setValue(128)
        self.addKnob(self.valHiddenKnob)

        self.addKnob(nuke.Text_Knob("div4", "", ""))

        # ── Encoder path ──
        self.encoderPathKnob = nuke.File_Knob("encoder_path", "Encoder Exe")
        # Try to find nvdb_encode.exe
        default_paths = [
            os.path.join(os.path.dirname(__file__), "nvdb_encode.exe"),
            os.path.expanduser("~/.nuke/plugins/VDBRender/nvdb_encode.exe"),
            r"C:\dev\VDBmarcher\build_neural\nvdb_encode.exe",
            r"C:\dev\VDBmarcher\nvdb_encode.exe",
        ]
        for p in default_paths:
            if os.path.isfile(p):
                self.encoderPathKnob.setValue(p)
                break
        self.addKnob(self.encoderPathKnob)

        # ── LibTorch path (for PATH) ──
        self.libtorchPathKnob = nuke.File_Knob("libtorch_path", "LibTorch lib/")
        libtorch_default = r"C:\libtorch\lib"
        if os.path.isdir(libtorch_default):
            self.libtorchPathKnob.setValue(libtorch_default)
        self.addKnob(self.libtorchPathKnob)

        self.addKnob(nuke.Text_Knob("div5", "", ""))

        # ── Controls ──
        self.startBtn = nuke.PyScript_Knob("start_encode", "Start Encoding")
        self.startBtn.setFlag(nuke.STARTLINE)
        self.addKnob(self.startBtn)

        self.cancelBtn = nuke.PyScript_Knob("cancel_encode", "Cancel")
        self.addKnob(self.cancelBtn)

        # ── Status ──
        self.addKnob(nuke.Text_Knob("div6", "", ""))

        self.statusKnob = nuke.Text_Knob("status", "Status", "Ready")
        self.addKnob(self.statusKnob)

        self.progressKnob = nuke.Text_Knob("progress", "Progress", "")
        self.addKnob(self.progressKnob)

        self.logKnob = nuke.Multiline_Eval_String_Knob("log", "Log")
        self.logKnob.setValue("")
        self.addKnob(self.logKnob)

        # ── Internal state ──
        self._process = None
        self._thread = None
        self._log_lines = []
        self._running = False
        self._cancel_requested = False

    def knobChanged(self, knob):
        if knob is self.fromNodeKnob:
            self._fillFromNode()
        elif knob is self.startBtn:
            self._startEncoding()
        elif knob is self.cancelBtn:
            self._cancelEncoding()
        elif knob is self.inputKnob:
            # Auto-fill output path
            inp = self.inputKnob.value()
            if inp and not self.outputKnob.value():
                out = inp.replace(".vdb", ".nvdb").replace(".VDB", ".NVDB")
                self.outputKnob.setValue(out)

    def _fillFromNode(self):
        """Fill input path from selected VDBRender node."""
        node = nuke.selectedNode()
        if not node:
            nuke.message("Select a VDBRender node first.")
            return
        if node.Class() != "VDBRender":
            nuke.message("Selected node is not VDBRender.")
            return

        file_knob = node.knob("file")
        if file_knob:
            path = file_knob.value()
            self.inputKnob.setValue(path)
            out = path.replace(".vdb", ".nvdb").replace(".VDB", ".NVDB")
            self.outputKnob.setValue(out)

            # Check if it looks like a sequence
            if "####" in path or "%04d" in path:
                self.seqKnob.setValue(True)
                self.startKnob.setValue(int(nuke.root().firstFrame()))
                self.endKnob.setValue(int(nuke.root().lastFrame()))

    def _startEncoding(self):
        """Launch nvdb_encode.exe as background subprocess."""
        if self._running:
            nuke.message("Encoding already in progress.")
            return

        encoder = self.encoderPathKnob.value()
        if not encoder or not os.path.isfile(encoder):
            nuke.message("nvdb_encode.exe not found.\nSet the Encoder Exe path.")
            return

        inp = self.inputKnob.value()
        out = self.outputKnob.value()
        if not inp or not out:
            nuke.message("Set input and output file paths.")
            return

        # Build command
        cmd = [encoder, "--input", inp, "--output", out]

        if self.seqKnob.value():
            cmd += ["--start", str(self.startKnob.value())]
            cmd += ["--end", str(self.endKnob.value())]

        if self.warmStartKnob.value() and self.seqKnob.value():
            cmd += ["--warm-start"]

        cmd += ["--topo-epochs", str(self.topoEpochsKnob.value())]
        cmd += ["--value-epochs", str(self.valEpochsKnob.value())]
        cmd += ["--value-hidden", str(self.valHiddenKnob.value())]

        # Set up environment with LibTorch on PATH
        env = os.environ.copy()
        libtorch_lib = self.libtorchPathKnob.value()
        if libtorch_lib and os.path.isdir(libtorch_lib):
            env["PATH"] = libtorch_lib + os.pathsep + env.get("PATH", "")

        # Clear log
        self._log_lines = []
        self._cancel_requested = False
        self.logKnob.setValue("")
        self.statusKnob.setValue("<font color='#4a9'>Encoding...</font>")
        self.progressKnob.setValue("")

        # Launch subprocess in background thread
        self._running = True

        def run_encoder():
            try:
                self._process = subprocess.Popen(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    env=env,
                    creationflags=subprocess.CREATE_NO_WINDOW if hasattr(subprocess, 'CREATE_NO_WINDOW') else 0,
                    bufsize=1,
                    universal_newlines=True
                )

                for line in iter(self._process.stdout.readline, ''):
                    if self._cancel_requested:
                        self._process.terminate()
                        self._appendLog("[Cancelled by user]")
                        break

                    line = line.rstrip()
                    if line:
                        self._appendLog(line)
                        self._parseProgress(line)

                self._process.stdout.close()
                retcode = self._process.wait()

                if self._cancel_requested:
                    nuke.executeInMainThread(self._setStatus,
                        args=("<font color='#e94'>Cancelled</font>",))
                elif retcode == 0:
                    nuke.executeInMainThread(self._setStatus,
                        args=("<font color='#4a9'>Complete!</font>",))
                else:
                    nuke.executeInMainThread(self._setStatus,
                        args=("<font color='#e44'>Failed (code %d)</font>" % retcode,))

            except Exception as e:
                self._appendLog("ERROR: %s" % str(e))
                nuke.executeInMainThread(self._setStatus,
                    args=("<font color='#e44'>Error: %s</font>" % str(e),))
            finally:
                self._process = None
                self._running = False

        self._thread = threading.Thread(target=run_encoder, daemon=True)
        self._thread.start()

    def _cancelEncoding(self):
        """Request cancellation of running encode."""
        if self._running and self._process:
            self._cancel_requested = True

    def _appendLog(self, line):
        """Thread-safe log append."""
        self._log_lines.append(line)
        # Keep last 200 lines
        if len(self._log_lines) > 200:
            self._log_lines = self._log_lines[-200:]
        log_text = "\n".join(self._log_lines[-50:])  # Show last 50 in UI
        nuke.executeInMainThread(self._setLog, args=(log_text,))

    def _parseProgress(self, line):
        """Extract progress info from encoder output."""
        # Frame progress: "--- Frame 42 ---"
        m = re.search(r'Frame\s+(\d+)', line)
        if m and self.seqKnob.value():
            frame = int(m.group(1))
            start = self.startKnob.value()
            end = self.endKnob.value()
            total = end - start + 1
            done = frame - start
            pct = int(100 * done / max(total, 1))
            nuke.executeInMainThread(self._setProgress,
                args=("Frame %d/%d (%d%%)" % (frame, end, pct),))

        # Topology accuracy
        m = re.search(r'Best topology accuracy:\s+([\d.]+)%', line)
        if m:
            nuke.executeInMainThread(self._setProgress,
                args=("Topo: %s%% — training values..." % m.group(1),))

        # PSNR
        m = re.search(r'Best PSNR:\s+([\d.]+)', line)
        if m:
            nuke.executeInMainThread(self._setProgress,
                args=("PSNR: %s dB — writing file..." % m.group(1),))

        # Ratio
        m = re.search(r'Ratio:\s+([\d.]+)x', line)
        if m:
            nuke.executeInMainThread(self._setProgress,
                args=("Compressed %.1fx" % float(m.group(1)),))

        # Single frame completion
        if "Written:" in line:
            nuke.executeInMainThread(self._setProgress,
                args=("Saved: " + line.split("Written:")[-1].strip(),))

    def _setStatus(self, text):
        """Main thread: update status knob."""
        try:
            self.statusKnob.setValue(text)
        except:
            pass

    def _setProgress(self, text):
        """Main thread: update progress knob."""
        try:
            self.progressKnob.setValue(text)
        except:
            pass

    def _setLog(self, text):
        """Main thread: update log knob."""
        try:
            self.logKnob.setValue(text)
        except:
            pass


# ── Panel registration ──

_panel_instance = None

def show_panel():
    """Show the encoder panel (singleton)."""
    global _panel_instance
    if _panel_instance is None:
        _panel_instance = NVDBEncoderPanel()
    _panel_instance.show()

def register():
    """Register in Nuke's menu."""
    toolbar = nuke.toolbar("Nodes")
    vdb_menu = toolbar.findItem("VDBRender")
    if not vdb_menu:
        vdb_menu = toolbar.addMenu("VDBRender", icon="VDBRender.png")
    vdb_menu.addCommand("NeuralVDB Encoder", show_panel)

    # Also add to menu bar
    nuke.menu("Nuke").addCommand("VDBRender/NeuralVDB Encoder", show_panel)
