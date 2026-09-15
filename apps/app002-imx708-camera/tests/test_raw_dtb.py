# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "prepare_raw", Path(__file__).resolve().parents[1] / "scripts/prepare-raw-dtb.py")
RAW = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RAW)


@unittest.skipUnless(all(shutil.which(t) for t in ("dtc", "fdtoverlay", "fdtget", "fdtput")),
                     "device-tree tools required")
class RawDtbTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.base = self.directory / "base.dtb"
        source = self.directory / "base.dts"
        source.write_text('''/dts-v1/;
        / {
            compatible = "radxa,dragon-q6a";
            tlmm: gpio { gpio-controller; #gpio-cells = <2>; };
            vreg_l10c_0p88: supply-a {};
            vreg_l6b_1p2: supply-b {};
            cci0: cci-a { status = "okay"; };
            cci1: cci-b { status = "okay"; };
            camss: camera { status = "okay"; };
        };''')
        subprocess.run(["dtc", "-@", "-o", str(self.base), str(source)], check=True)

    def test_bus_stage_does_not_enable_sensor_or_hardware_camera(self):
        output = self.directory / "bus.dtb"
        RAW.prepare(self.base, output, "bus")
        for node in ("/cci-a", "/cci-b", "/camera", "/i2c-cam3-gpio/imx708@1a"):
            self.assertEqual(RAW.run("fdtget", output, node, "status"), "disabled")
        self.assertEqual(RAW.run("fdtget", output, "/i2c-cam3-gpio", "status"), "okay")

    def test_sensor_stage_preserves_base_and_keeps_camss_disabled(self):
        before = self.base.read_bytes()
        output = self.directory / "sensor.dtb"
        RAW.prepare(self.base, output, "sensor")
        self.assertEqual(before, self.base.read_bytes())
        self.assertEqual(RAW.run("fdtget", output, "/i2c-cam3-gpio/imx708@1a", "status"), "okay")
        self.assertEqual(RAW.run("fdtget", output, "/camera", "status"), "disabled")

    def test_refuses_overwrite(self):
        with self.assertRaises(ValueError):
            RAW.prepare(self.base, self.base, "sensor")

    def test_camss_stage_keeps_native_cci_disabled(self):
        output = self.directory / "camss.dtb"
        RAW.prepare(self.base, output, "camss")
        self.assertEqual(RAW.run("fdtget", output, "/camera", "status"), "okay")
        for node in ("/cci-a", "/cci-b"):
            self.assertEqual(RAW.run("fdtget", output, node, "status"), "disabled")
        for supply in ("vdda-phy-supply", "vdda-pll-supply"):
            self.assertTrue(RAW.run("fdtget", output, "/camera", supply))
        receiver = "/camera/ports/port@3/endpoint"
        self.assertEqual(RAW.run("fdtget", output, receiver, "clock-lanes"), "7")
        self.assertEqual(RAW.run("fdtget", output, receiver, "data-lanes"), "0 1")

    def test_production_cci_include_and_conflicts(self):
        import sys
        sys.path.insert(0, str(Path(__file__).resolve().parents[2]/"lib"))
        from qcom_apps.native_camera import audit_dtb
        source = self.directory/"base.dts"
        include = Path(__file__).resolve().parents[1]/"kernel/dts/q6a-imx708-native.dtsi"
        fixture = source.read_text().replace('cci1: cci-b { status = "okay"; };',
            '''cci3_default: pins-active { pins = "gpio75", "gpio76"; };
            cci3_sleep: pins-sleep { pins = "gpio75", "gpio76"; };
            cci1: cci-b { status = "okay"; compatible = "qcom,sc7280-cci";
                cci1_i2c0: i2c-bus@0 { reg = <0>; };
                cci1_i2c1: i2c-bus@1 { reg = <1>; #address-cells = <1>; #size-cells = <0>; };
            };''')
        source.write_text(fixture + include.read_text())
        output = self.directory/"production.dtb"
        subprocess.run(["dtc", "-@", "-o", str(output), str(source)],check=True)
        audit_dtb(output)
        original = output.read_bytes()
        for node, prop, value, kind in [
            ("/cci-a", "status", "okay", "s"),
            ("/cci-b/i2c-bus@0", "status", "okay", "s"),
            ("/cci-b/i2c-bus@1", "clock-frequency", "400000", "u"),
            ("/cci-b/i2c-bus@1/imx708@1a", "reg", "12", "u"),
            ("/cci-b", "pinctrl-0", "0", "u"),
        ]:
            output.write_bytes(original)
            RAW.run("fdtput", "-t", kind, output, node, prop, value)
            with self.assertRaises(ValueError): audit_dtb(output)

    def test_refuses_other_board(self):
        RAW.run("fdtput", "-t", "s", self.base, "/", "compatible", "other,board")
        with self.assertRaises(ValueError):
            RAW.prepare(self.base, self.directory / "wrong.dtb", "sensor")


if __name__ == "__main__":
    unittest.main()
