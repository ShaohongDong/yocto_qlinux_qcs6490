# SPDX-License-Identifier: MIT
"""Development opt-in contract, including the actual BitBake task bodies."""

import contextlib
import importlib.machinery
import importlib.util
import io
import re
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
loader = importlib.machinery.SourceFileLoader("qcom_app_cli", str(ROOT / "scripts/qcom-app"))
spec = importlib.util.spec_from_loader(loader.name, loader)
cli = importlib.util.module_from_spec(spec)
loader.exec_module(cli)


class DevelopmentImageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.values = {
            "QCOM_APP": "imx708-camera", "MACHINE": "radxa-dragon-q6a",
            "QCOM_IMX708_BACKEND": "camx",
            "QCOM_APP_IMAGE_ACTIVE": "1", "QCOM_IMX708_ALLOW_INCOMPLETE": "0",
            "QCOM_IMX708_INCOMPLETE_REASON": "Sensor integration pending.",
            "IMAGE_ROOTFS": self.temp.name,
        }
        self.bb = SimpleNamespace(fatal=Mock(side_effect=RuntimeError), warn=Mock())

    def task(self, name):
        source = (ROOT / "apps/classes/qcom-imx708-image-gate.bbclass").read_text()
        body = re.search(r"python " + name + r"\(\) \{\n(.*?)\n\}", source, re.S).group(1)
        namespace = {"bb": self.bb, "d": SimpleNamespace(getVar=self.values.get)}
        exec("def task():\n" + body, namespace)
        namespace["task"]()

    def test_default_blocks_and_explicit_development_writes_marker(self):
        with self.assertRaises(RuntimeError):
            self.task("do_imx708_support_check")
        self.values["QCOM_IMX708_ALLOW_INCOMPLETE"] = "1"
        self.task("do_imx708_support_check")
        self.bb.warn.assert_called_once()
        self.task("imx708_development_marker")
        marker = Path(self.temp.name) / "etc/imx708-camera-development"
        self.assertIn("real camera capture is not validated", marker.read_text())
        self.assertIn("Sensor integration pending", marker.read_text())

    def test_native_image_uses_manifest_contract_without_development_marker(self):
        self.values.update(QCOM_IMX708_BACKEND="native", QCOM_APPS_DIR=str(ROOT/"apps"))
        self.task("do_imx708_support_check")
        self.task("imx708_development_marker")
        self.bb.warn.assert_not_called()
        self.assertFalse((Path(self.temp.name)/"etc/imx708-camera-development").exists())

    def test_native_contract_rejects_missing_services(self):
        from qcom_apps.manifest import load_manifest
        from qcom_apps.native_camera import validate_manifest
        spec = load_manifest(ROOT/"apps", "imx708-camera")
        with self.assertRaises(ValueError):
            validate_manifest(SimpleNamespace(kernel=spec.kernel, services=[]))

    def test_native_artifact_gate_rejects_empty_rootfs(self):
        self.values.update(QCOM_IMX708_BACKEND="native", DEPLOY_DIR_IMAGE=self.temp.name,
                           STAGING_BINDIR_NATIVE=self.temp.name)
        with patch("qcom_apps.native_camera.audit_dtb") as dtb:
            with self.assertRaises(RuntimeError):
                self.task("do_imx708_native_image_check")
            dtb.assert_called_once()

    def test_unselected_native_image_skips_artifact_gate(self):
        self.values.update(QCOM_IMX708_BACKEND="native", QCOM_APP_IMAGE_ACTIVE="0")
        with patch("qcom_apps.native_camera.audit_rootfs") as rootfs:
            self.task("do_imx708_native_image_check")
            rootfs.assert_not_called()

    def test_other_machine_cannot_bypass_gate(self):
        self.values.update(MACHINE="radxa-dragon-q8b", QCOM_IMX708_ALLOW_INCOMPLETE="1")
        with self.assertRaises(RuntimeError):
            self.task("do_imx708_support_check")

    def test_unselected_image_has_no_warning_or_marker(self):
        self.values.update(QCOM_APP_IMAGE_ACTIVE="0", QCOM_IMX708_ALLOW_INCOMPLETE="1")
        self.task("do_imx708_support_check")
        self.task("imx708_development_marker")
        self.bb.warn.assert_not_called()
        self.assertFalse((Path(self.temp.name) / "etc").exists())

    def test_cli_rejects_wrong_app_machine_and_component_command(self):
        for action, app, machine in (
            ("image", "example", "radxa-dragon-q6a"),
            ("all", "imx708-camera", "radxa-dragon-q8b"),
            ("build", "imx708-camera", "radxa-dragon-q6a"),
        ):
            with self.subTest(action=action, app=app, machine=machine):
                with patch("sys.argv", ["qcom-app", action, "--app", app, "--machine", machine,
                                        "--allow-incomplete-camera"]), contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        cli.main()
                    self.assertEqual(error.exception.code, 2)

    def test_cli_passes_opt_in_only_through_temporary_config(self):
        for enabled in (False, True):
            args = SimpleNamespace(app="imx708-camera", machine="radxa-dragon-q6a",
                                   allow_incomplete_camera=enabled, dry_run=False)

            def run(command, **kwargs):
                content = Path(command[2]).read_text()
                self.assertEqual('QCOM_IMX708_ALLOW_INCOMPLETE = "1"' in content, enabled)
                self.assertIn('QCOM_APP = "imx708-camera"', content)
                return SimpleNamespace(returncode=0)

            with patch.dict("os.environ", {"OECORE_NATIVE_SYSROOT": "/sdk"}), patch.object(cli.subprocess, "run", side_effect=run):
                self.assertEqual(cli._run_bitbake(args, ["image-target"], "image-target"), 0)


if __name__ == "__main__":
    unittest.main()
