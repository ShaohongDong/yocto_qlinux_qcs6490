# SPDX-License-Identifier: MIT
"""Unit tests for the QCOM application manifest contract."""

from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "apps/lib"))

from qcom_apps import AppManifest, ManifestError


class AppManifestTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name) / "example"
        shutil.copytree(ROOT / "apps/example", self.root)

    def tearDown(self):
        self.temporary.cleanup()

    @property
    def manifest(self) -> Path:
        return self.root / "app.yaml"

    def load_data(self):
        return yaml.safe_load(self.manifest.read_text(encoding="utf-8"))

    def save_data(self, data):
        self.manifest.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")

    def test_reference_manifest_is_valid(self):
        spec = AppManifest(self.manifest)
        self.assertEqual(spec.name, "example")
        self.assertEqual(spec.build["system"], "cmake")
        self.assertEqual(spec.image("radxa-dragon-q6a"), "qcom-minimal-efi-sd-image")
        self.assertIn("kernel/dts/qcom-app-example.dtsi", spec.expanded_inputs())
        self.assertIn(self.manifest, spec.parse_dependencies())
        self.assertIn(self.root / "kernel/dts", spec.parse_dependencies())

    def test_python_bytecode_does_not_change_build_inputs(self):
        before = AppManifest(self.manifest)
        cache = self.root / "src/__pycache__"
        cache.mkdir()
        bytecode = cache / "helper.cpython-312.pyc"
        bytecode.write_bytes(b"first interpreter")
        loose = self.root / "src/helper.pyc"
        loose.write_bytes(b"legacy bytecode")
        after = AppManifest(self.manifest)
        self.assertEqual(before.digest, after.digest)
        self.assertNotIn(bytecode, after.parse_dependencies())
        self.assertNotIn(cache, after.parse_dependencies())
        self.assertNotIn(loose, after.parse_dependencies())
        bytecode.write_bytes(b"different interpreter")
        self.assertEqual(after.digest, AppManifest(self.manifest).digest)
        (self.root / "src/helper.py").write_text("real_source = 1\n")
        self.assertNotEqual(after.digest, AppManifest(self.manifest).digest)

    def test_duplicate_and_unknown_keys_are_rejected(self):
        text = self.manifest.read_text(encoding="utf-8")
        self.manifest.write_text(text + "name: duplicate\n", encoding="utf-8")
        with self.assertRaisesRegex(ManifestError, "duplicate YAML key"):
            AppManifest(self.manifest)

        shutil.copy2(ROOT / "apps/example/app.yaml", self.manifest)
        data = self.load_data()
        data["unknown"] = True
        self.save_data(data)
        with self.assertRaisesRegex(ManifestError, "unknown keys"):
            AppManifest(self.manifest)

    def test_paths_and_symlinks_cannot_escape(self):
        data = self.load_data()
        data["inputs"].append("../outside")
        self.save_data(data)
        with self.assertRaisesRegex(ManifestError, "unsafe path characters"):
            AppManifest(self.manifest)

        shutil.copy2(ROOT / "apps/example/app.yaml", self.manifest)
        (self.root / "src/escape").symlink_to("/etc/passwd")
        with self.assertRaisesRegex(ManifestError, "symbolic links"):
            AppManifest(self.manifest)

        (self.root / "src/escape").unlink()
        data = self.load_data()
        data["kernel"]["modules"][0] = "kernel/modules/example&run"
        self.save_data(data)
        with self.assertRaisesRegex(ManifestError, "unsafe path characters"):
            AppManifest(self.manifest)

    def test_schema_version_and_check_path_types_are_strict(self):
        data = self.load_data()
        data["schema_version"] = True
        self.save_data(data)
        with self.assertRaisesRegex(ManifestError, "integer 1"):
            AppManifest(self.manifest)

        shutil.copy2(ROOT / "apps/example/app.yaml", self.manifest)
        data = self.load_data()
        data["check"][0] = "/usr/bin/app'unsafe"
        self.save_data(data)
        with self.assertRaisesRegex(ManifestError, "unsafe or non-normalized"):
            AppManifest(self.manifest)

    def test_license_checksum_is_verified(self):
        data = self.load_data()
        data["license"]["files"][0]["md5"] = "f" * 32
        self.save_data(data)
        with self.assertRaisesRegex(ManifestError, "license checksum mismatch"):
            AppManifest(self.manifest)

    def test_machine_provider_and_image_are_strict(self):
        spec = AppManifest(self.manifest)
        with self.assertRaisesRegex(ManifestError, "does not support machine"):
            spec.machine("radxa-dragon-q8b")
        with self.assertRaisesRegex(ManifestError, "is not allowed"):
            spec.image("radxa-dragon-q6a", "core-image-minimal")

    def test_meson_manifest_is_supported(self):
        data = self.load_data()
        data["build"]["system"] = "meson"
        data["inputs"].remove("CMakeLists.txt")
        data["inputs"].append("meson.build")
        (self.root / "meson.build").write_text(
            "project('example', 'cpp')\nexecutable('qcom_app_example', 'src/main.cpp', install: true)\n",
            encoding="utf-8",
        )
        self.save_data(data)
        self.assertEqual(AppManifest(self.manifest).build["system"], "meson")

    def test_service_wanted_by_and_module_makefile_are_checked(self):
        service = self.root / "systemd/qcom-app-example.service"
        service.write_text(service.read_text(encoding="utf-8").replace(
            "WantedBy=multi-user.target", "WantedBy=network.target"), encoding="utf-8")
        with self.assertRaisesRegex(ManifestError, "WantedBy"):
            AppManifest(self.manifest)

        shutil.copy2(ROOT / "apps/example/systemd/qcom-app-example.service", service)
        (self.root / "kernel/modules/example/Makefile").unlink()
        with self.assertRaisesRegex(ManifestError, "lacks Makefile or Kbuild"):
            AppManifest(self.manifest)


if __name__ == "__main__":
    unittest.main()
