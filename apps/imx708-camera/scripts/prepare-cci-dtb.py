#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Create a CCI1/master1-only diagnostic DTB without installing it.

Native CAMSS, GPIO I2C and sensor binding are disabled. Load only a CCI module
whose controller-wide completion initialization has been verified.
"""

import argparse
import subprocess
import tempfile
from pathlib import Path


def run(*args):
    return subprocess.check_output([str(x) for x in args], text=True).strip()


def prepare(base, output):
    base, output = Path(base), Path(output)
    if output.exists() or base.resolve() == output.resolve():
        raise ValueError("output must be a new file distinct from the base")
    if "radxa,dragon-q6a" not in run("fdtget", base, "/", "compatible").split():
        raise ValueError("base must be Q6A")
    symbols = {
        label: run("fdtget", base, "/__symbols__", label)
        for label in (
            "cci0",
            "cci1",
            "cci1_i2c0",
            "cci1_i2c1",
            "camss",
            "cci3_default",
            "cci3_sleep",
        )
    }
    with tempfile.TemporaryDirectory() as directory:
        temp = Path(directory) / "cci.dtb"
        temp.write_bytes(base.read_bytes())
        for label in ("cci0", "camss", "cci1_i2c0"):
            run("fdtput", "-t", "s", temp, symbols[label], "status", "disabled")
        if "i2c-cam3-gpio" in run("fdtget", "-l", temp, "/").split():
            run("fdtput", "-t", "s", temp, "/i2c-cam3-gpio", "status", "disabled")
            for child in run("fdtget", "-l", temp, "/i2c-cam3-gpio").split():
                run(
                    "fdtput",
                    "-t",
                    "s",
                    temp,
                    "/i2c-cam3-gpio/" + child,
                    "status",
                    "disabled",
                )
        for label in ("cci1", "cci1_i2c1"):
            run("fdtput", "-t", "s", temp, symbols[label], "status", "okay")
        for label in ("cci1_i2c0", "cci1_i2c1"):
            for child in run("fdtget", "-l", temp, symbols[label]).splitlines():
                run(
                    "fdtput",
                    "-t",
                    "s",
                    temp,
                    symbols[label] + "/" + child,
                    "status",
                    "disabled",
                )
        for prop, label in [("pinctrl-0", "cci3_default"), ("pinctrl-1", "cci3_sleep")]:
            phandle = run("fdtget", "-t", "x", temp, symbols[label], "phandle")
            run("fdtput", "-t", "x", temp, symbols["cci1"], prop, phandle)
        run(
            "fdtput", "-t", "i", temp, symbols["cci1_i2c1"], "clock-frequency", "100000"
        )
        run("fdtput", "-t", "s", temp, "/aliases", "i2c19", symbols["cci1_i2c1"])
        assert run("fdtget", temp, symbols["cci1"], "status") == "okay"
        assert run("fdtget", temp, symbols["cci1_i2c0"], "status") == "disabled"
        assert run("fdtget", temp, symbols["camss"], "status") == "disabled"
        with output.open("xb") as stream:
            stream.write(temp.read_bytes())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    prepare(args.base, args.output)
