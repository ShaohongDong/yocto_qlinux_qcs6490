#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.machinery
import importlib.util
import tempfile
import unittest
from pathlib import Path

loader = importlib.machinery.SourceFileLoader(
    "inventory", str(Path(__file__).parents[1] / "scripts/q6a-openhd-check"))
module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(module)


class InventoryTests(unittest.TestCase):
    def fixture(self, root, enabled=True, monitor=True):
        files = {
            "sys/firmware/devicetree/base/model": "Radxa Dragon Q6A\x00",
            "sys/class/net/wlan0/device/idVendor": "a69c\n",
            "sys/class/net/wlan0/device/idProduct": "8d81\n",
            "sys/class/net/wlan0/type": "803" if monitor else "1",
            "sys/module/aic8800_fdrv/parameters/openhd_monitor": "Y" if enabled else "N",
            "lib/firmware/aic8800_fw/USB/aic8800D80/test.bin": "test firmware",
        }
        for name, text in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)

    def run_command(self, argv):
        return {"exit_code": 0, "stdout": "Interface wlan0\n type monitor\n" if argv[:2] == ["iw", "dev"] else "", "stderr": ""}

    def test_positive_prerequisites_never_qualify_wireless(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.fixture(root)
            report = module.collect(root, "wlan0", "raw", self.run_command)
            self.assertEqual(report["result"], "PREREQUISITES_ONLY")
            self.assertFalse(report["wireless_qualified"])
            self.assertIn("openhd_fec_throughput", report["pending"])
            self.assertEqual(len(report["firmware_sha256"]), 1)

    def test_raw_requires_loaded_feature_and_interface_mode(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.fixture(root, enabled=False, monitor=False)
            report = module.collect(root, "wlan0", "raw", self.run_command)
            self.assertIn("monitor_tx_not_compiled", report["prerequisite_errors"])
            self.assertIn("interface_not_monitor", report["prerequisite_errors"])

    def test_wrong_board_and_usb_are_rejected(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            self.fixture(root)
            (root / "sys/firmware/devicetree/base/model").write_text("Some other board")
            (root / "sys/class/net/wlan0/device/idProduct").write_text("8d80")
            report = module.collect(root, "wlan0", "inventory", self.run_command)
            self.assertIn("unsupported_board", report["prerequisite_errors"])
            self.assertIn("unverified_usb_identity", report["prerequisite_errors"])


if __name__ == "__main__":
    unittest.main()
