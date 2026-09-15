#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise desktop health failures without requiring a running X server."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "recipes-graphics/xfce/files/qcom-xfce-ready"


class DesktopHealthTests(unittest.TestCase):
    def run_health(self, wm=True, missing="", renderer="Adreno (TM) 635", accelerated=True, glamor=True, gl_error=False, glx_exit=0, presentation=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            runtime = root / "qcom-xfce"
            runtime.mkdir()
            (runtime / "Xorg.0.log").write_text(
                ("glamor X acceleration enabled on Adreno (TM) 635\n" if glamor else "glamor initialization failed\n")
                + ("GL error: invalid shader\n" if gl_error else "")
            )
            commands = {
                "xprop": "echo '" + ("_NET_SUPPORTING_WM_CHECK(WINDOW): window id # 0x123" if wm else "_NET_SUPPORTING_WM_CHECK: not found.") + "'\n",
                "pgrep": 'for arg do last="$arg"; done\n[ "$last" != "$MISSING_PROCESS" ]\n',
                "sleep": "exit 0\n",
                "qcom-xfce-graphics-check": "exit " + ("0" if presentation else "1") + "\n",
                "glxinfo": "printf '%s\\n' 'direct rendering: Yes' 'Accelerated: " + ("yes" if accelerated else "no") + "' 'OpenGL renderer string: " + renderer + "'\nexit " + str(glx_exit) + "\n",

            }
            for name, body in commands.items():
                command = root / name
                command.write_text("#!/bin/sh\n" + body)
                command.chmod(0o755)
            env = dict(os.environ, PATH=directory + ":" + os.environ["PATH"], MISSING_PROCESS=missing, XDG_RUNTIME_DIR=directory)
            return subprocess.run(["sh", str(SCRIPT)], env=env, capture_output=True, text=True, timeout=10)

    def test_ready_session_passes(self):
        self.assertEqual(self.run_health().returncode, 0)

    def test_missing_window_manager_property_fails(self):
        result = self.run_health(wm=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("did not become ready", result.stderr)

    def test_missing_desktop_process_fails(self):
        for process in ("xfce4-session", "xfwm4", "xfce4-panel"):
            with self.subTest(process=process):
                self.assertNotEqual(self.run_health(missing=process).returncode, 0)

    def test_software_window_renderer_fails(self):
        for renderer in ("llvmpipe", "softpipe", "Adreno llvmpipe"):
            with self.subTest(renderer=renderer):
                self.assertNotEqual(self.run_health(renderer=renderer).returncode, 0)

    def test_unaccelerated_or_failed_glx_fails(self):
        self.assertNotEqual(self.run_health(accelerated=False).returncode, 0)
        self.assertNotEqual(self.run_health(glx_exit=1).returncode, 0)

    def test_black_window_presentation_fails(self):
        self.assertNotEqual(self.run_health(presentation=False).returncode, 0)

    def test_failed_glamor_or_gl_error_fails(self):
        self.assertNotEqual(self.run_health(glamor=False).returncode, 0)
        self.assertNotEqual(self.run_health(gl_error=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
