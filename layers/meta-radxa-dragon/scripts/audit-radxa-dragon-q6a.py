#!/usr/bin/env python3
"""Offline structural audit for Radxa Dragon Q6A Yocto artifacts."""

import sys

from radxa_dragon_audit import cli


PROFILE = {
    "name": "Q6A",
    "machine": "radxa-dragon-q6a",
    "boot_mode": "embloader-bls",
    "dtb": "qcs6490-radxa-dragon-q6a.dtb",
    "compatibles": ("radxa,dragon-q6a", "qcom,qcm6490"),
    "bios_version": "260815",
    "bios_board": "dragon-q6a",
    "bios_required": (
        "prog_firehose_ddr.elf",
        "rawprogram0.xml",
        "patch0.xml",
        "gpt_main0.bin",
        "gpt_backup0.bin",
        "xbl.elf",
        "xbl_config.elf",
        "imagefv.elf",
        "PILFV.Fv",
    ),
}


if __name__ == "__main__":
    sys.exit(cli(PROFILE))
