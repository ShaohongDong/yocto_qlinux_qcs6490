# SPDX-License-Identifier: MIT
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "cci_dtb", Path(__file__).resolve().parents[1] / "scripts/prepare-cci-dtb.py"
)
cci = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cci)


@unittest.skipUnless(
    all(shutil.which(x) for x in ("dtc", "fdtget", "fdtput")), "DT tools required"
)
class CciDtbTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.base = self.root / "base.dtb"
        dts = self.root / "base.dts"
        dts.write_text("""/dts-v1/;
/ { compatible="radxa,dragon-q6a"; aliases { i2c19=&cci1_i2c0; };
 cci3_default: pins-default {}; cci3_sleep: pins-sleep {};
 cci0: cci0 { status="okay"; };
 cci1: cci1 { status="disabled"; pinctrl-0=<&cci3_default>; pinctrl-1=<&cci3_sleep>;
  cci1_i2c0: i2c-bus@0 { status="okay"; };
  cci1_i2c1: i2c-bus@1 { status="disabled"; sensor@1a { status="okay"; }; };
 };
 camss: camera { status="okay"; };
 i2c-cam3-gpio { status="okay"; imx708@1a { status="okay"; }; };
};""")
        subprocess.run(
            ["dtc", "-@", "-o", str(self.base), str(dts)],
            check=True,
            stderr=subprocess.DEVNULL,
        )

    def test_isolates_master_one_and_preserves_original(self):
        before = self.base.read_bytes()
        out = self.root / "diag.dtb"
        cci.prepare(self.base, out)
        self.assertEqual(before, self.base.read_bytes())
        for node in (
            "/cci0",
            "/camera",
            "/cci1/i2c-bus@0",
            "/cci1/i2c-bus@1/sensor@1a",
            "/i2c-cam3-gpio",
            "/i2c-cam3-gpio/imx708@1a",
        ):
            self.assertEqual(cci.run("fdtget", out, node, "status"), "disabled")
        self.assertEqual(cci.run("fdtget", out, "/cci1/i2c-bus@1", "status"), "okay")
        self.assertEqual(
            cci.run("fdtget", "-t", "i", out, "/cci1/i2c-bus@1", "clock-frequency"),
            "100000",
        )
        self.assertEqual(cci.run("fdtget", out, "/aliases", "i2c19"), "/cci1/i2c-bus@1")

    def test_refuses_existing_output_and_other_board(self):
        with self.assertRaises(ValueError):
            cci.prepare(self.base, self.base)
        cci.run("fdtput", "-t", "s", self.base, "/", "compatible", "other,board")
        with self.assertRaises(ValueError):
            cci.prepare(self.base, self.root / "other.dtb")


if __name__ == "__main__":
    unittest.main()
