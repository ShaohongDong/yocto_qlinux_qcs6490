#!/usr/bin/env python3
"""Offline structural audit for Radxa Dragon Q8B Yocto artifacts."""

import sys

from radxa_dragon_audit import cli


PROFILE = {
    "name": "Q8B",
    "machine": "radxa-dragon-q8b",
    "dtb": "sc8280xp-radxa-dragon-q8b.dtb",
    "compatibles": ("radxa,dragon-q8b", "qcom,sc8280xp"),
    "bios_version": "260825",
    "bios_board": "dragon-q8b",
    "bios_required": (
        "prog_firehose_ddr.elf",
        "prog_firehose_lite.elf",
        "rawprogram0.xml",
        "patch0.xml",
        "gpt_main0.bin",
        "gpt_backup0.bin",
        "xbl.elf",
        "xbl_config.elf",
        "imagefv.elf",
        "tz.mbn",
        "hyp.mbn",
        "aop.mbn",
        "cpucp.elf",
        "shrm.elf",
        "qupv3fw.elf",
        "usb4_uc_fw_image.elf",
        "eth_fw.bin",
    ),
    "rootfs_paths": (
        "/lib/firmware/qcom/sc8280xp/LENOVO/21BX/qcdxkmsuc8280.mbn",
        "/lib/firmware/qcom/sc8280xp/qccdsp8280.mbn",
        "/lib/firmware/qcom/sc8280xp/qupv3fw.elf",
        "/lib/firmware/qcom/sc8280xp/radxa/dragon-q8b/qcadsp8280.mbn",
        "/lib/firmware/qcom/vpu/vpu20_p4_gen2_s6.mbn",
        "/usr/share/qcom/sc8280xp/radxa/dragon-q8b/dsp/adsp/fastrpc_shell_0",
        "/usr/share/qcom/sc8280xp/radxa/dragon-q8b/dsp/cdsp/fastrpc_shell_3",
        "/usr/share/qcom/conf.d/hexagon-dsp-binaries-radxa-dragon-q8b.yaml",
    ),
    "manifest": {
        "kernel_version": "7.0.11-6-qcom",
        "kernel_source_revision": "657c0f722940cd9d3b51abfa7383655ec7d2c795",
        "kernel_packaging_revision": "69f6ffe75b23f31312fc94b1b7f748929d41d9c9",
        "firmware_version": "0.2.41",
        "firmware_source_revision": "e1761009df008adfd62c77f2c5584e3067449013",
    },
}


if __name__ == "__main__":
    sys.exit(cli(PROFILE))
