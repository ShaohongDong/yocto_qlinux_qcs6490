#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the inactive CAM3 candidate with the real Q6A board DTS, without stubs."""
import argparse
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel-source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--dtc", default="dtc")
    parser.add_argument("--fdtget", default="fdtget")
    args = parser.parse_args()
    kernel, out = args.kernel_source.resolve(), args.output.resolve()
    if out.exists() and any(out.iterdir()):
        parser.error("Use an empty output directory")
    candidate = Path(__file__).resolve().parents[1] / "kernel/q6a-cam3-camx-candidate.dtsi"
    out.mkdir(parents=True, exist_ok=True)
    wrapper = out / "candidate.dts"
    wrapper.write_text('#include "qcs6490-radxa-dragon-q6a.dts"\n#include "'
                       + str(candidate) + '"\n')
    pp = out / "candidate.pp.dts"
    with pp.open("w") as stream:
        subprocess.run(["gcc", "-E", "-nostdinc", "-undef", "-D__DTS__", "-x", "assembler-with-cpp",
                        "-I", str(kernel / "arch/arm64/boot/dts/qcom"),
                        "-I", str(kernel / "scripts/dtc/include-prefixes"), str(wrapper)],
                       stdout=stream, check=True)
    dtb = out / "candidate.dtb"
    with (out / "dtc.log").open("w") as log:
        subprocess.run([args.dtc, "-I", "dts", "-O", "dtb", "-o", str(dtb), str(pp)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    for node in ("/soc@0/cci@ac4b000", "/soc@0/cci@ac4b000/sensor-cam3",
                 "/soc@0/cci@ac4b000/actuator-cam3", "/soc@0/csiphy@ace6000"):
        value = subprocess.check_output([args.fdtget, "-t", "s", str(dtb), node, "status"], text=True).strip()
        if value != "disabled":
            raise RuntimeError(f"Unexpected enabled candidate node: {node}")
    print("PASS: candidate compiles against Q6A DTS; all candidate nodes remain disabled")
    print("This is not DT schema, driver probe or hardware capture acceptance.")


if __name__ == "__main__":
    main()
