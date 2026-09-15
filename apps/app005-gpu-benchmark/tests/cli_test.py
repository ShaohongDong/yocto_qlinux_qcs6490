# SPDX-License-Identifier: MIT
"""Exercise failure reports without a GPU or desktop."""
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

BINARY = sys.argv.pop(1)


class CliTests(unittest.TestCase):
    def test_model_decodes_without_gpu(self):
        path = Path(__file__).resolve().parents[1] / "data/models/FlightHelmet.glb"
        run = subprocess.run([BINARY, "--check-model", "--model", str(path)], capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertIn("triangles, 6 meshes", run.stdout)

    def test_corrupt_model_rejected(self):
        with tempfile.NamedTemporaryFile() as file:
            file.write(b"invalid glTF model" * 10)
            file.flush()
            run = subprocess.run([BINARY, "--check-model", "--model", file.name], capture_output=True)
            self.assertEqual(run.returncode, 2)

    def test_self_test_without_graphics(self):
        env = dict(os.environ, DISPLAY="", WAYLAND_DISPLAY="")
        self.assertEqual(subprocess.run([BINARY, "--self-test"], env=env).returncode, 0)

    def test_invalid_arguments(self):
        for args in (["--duration", "-1"], ["--suite", "missing"], ["--size", "1920x0"],
                     ["--gui", "--headless"], ["--window"], ["--unknown"], ["--output"],
                     ["--instances", "0"], ["--instances", "65"], ["--offscreen"]):
            with self.subTest(args=args):
                self.assertEqual(subprocess.run([BINARY, *args], capture_output=True).returncode, 2)

    def test_missing_gpu_writes_failure_not_baseline_pass(self):
        with tempfile.TemporaryDirectory() as root:
            run = subprocess.run([BINARY, "--headless", "--device", "/dev/dri/renderD9999",
                                  "--output", root], capture_output=True)
            self.assertEqual(run.returncode, 1)
            report = json.loads((Path(root) / "summary.json").read_text())
            self.assertEqual(report["status"], "failed")
            self.assertFalse(report["pixel_check_passed"])
            self.assertEqual(report["scenes"], [])
            self.assertTrue(report["error"])

    def test_unwritable_report_destination(self):
        with tempfile.NamedTemporaryFile() as file:
            run = subprocess.run([BINARY, "--headless", "--output", file.name], capture_output=True)
            self.assertEqual(run.returncode, 2)

    @unittest.skipUnless(shutil.which("xvfb-run"), "Xvfb unavailable")
    def test_gui_context_failure_exits_instead_of_waiting_for_render(self):
        with tempfile.TemporaryDirectory() as root:
            run = subprocess.run(["xvfb-run", "-a", BINARY, "--gui", "--window", "--autorun",
                                  "--quit-after-run", "--output", root],
                                 env=dict(os.environ, GDK_GL="disable"), capture_output=True, timeout=15)
            self.assertEqual(run.returncode, 1, run.stderr)
            reports = list(Path(root).glob("*/summary.json"))
            self.assertEqual(len(reports), 1)
            self.assertEqual(json.loads(reports[0].read_text())["status"], "failed")


if __name__ == "__main__":
    unittest.main()
