#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build an offline Q6A diagnostic DTB; never install it or alter boot entries.

Stages: bus (GPIO I2C only), sensor (also instantiate IMX708), camss (receiver).
Native CCI stays disabled. Use modprobe.blacklist=imx708,qcom_camss,i2c_qcom_cci
on diagnostic boots, then explicitly load each driver under serial capture.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(*args):
    return subprocess.check_output([str(arg) for arg in args], text=True).strip()


def prepare(base, output, stage):
    base, output = Path(base), Path(output)
    if stage not in ("bus", "sensor", "camss"):
        raise ValueError("unsupported diagnostic stage")
    if base.resolve() == output.resolve() or output.exists():
        raise ValueError("output must be a new file distinct from the base DTB")
    if "radxa,dragon-q6a" not in run("fdtget", "-t", "s", base, "/", "compatible").split():
        raise ValueError("base DTB is not Radxa Dragon Q6A")
    source = Path(__file__).resolve().parents[1] / "kernel/q6a-raw-gpio-i2c.dtso"
    with tempfile.TemporaryDirectory(prefix="q6a-raw-dtb-") as directory:
        overlay = Path(directory) / "raw.dtbo"
        merged = Path(directory) / "raw.dtb"
        subprocess.run(["dtc", "-@", "-I", "dts", "-O", "dtb", "-o",
                        str(overlay), str(source)], check=True)
        subprocess.run(["fdtoverlay", "-i", str(base), "-o", str(merged),
                        str(overlay)], check=True)
        if stage in ("sensor", "camss"):
            run("fdtput", "-t", "s", merged, "/i2c-cam3-gpio/imx708@1a",
                "status", "okay")
        if stage == "camss":
            node = run("fdtget", merged, "/__symbols__", "camss")
            for supply in ("vdda-phy-supply", "vdda-pll-supply"):
                run("fdtget", "-t", "x", merged, node, supply)
            run("fdtput", "-t", "s", merged, node, "status", "okay")
        for label in ("cci0", "cci1", "camss"):
            node = run("fdtget", merged, "/__symbols__", label)
            expected = "okay" if label == "camss" and stage == "camss" else "disabled"
            if run("fdtget", merged, node, "status") != expected:
                raise ValueError(f"unexpected {label} status")
        output.write_bytes(merged.read_bytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--stage", required=True, choices=("bus", "sensor", "camss"))
    args = parser.parse_args()
    try:
        prepare(args.base, args.output, args.stage)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
