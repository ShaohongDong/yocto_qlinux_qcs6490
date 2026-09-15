#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Inventory SDK camera inputs; missing assets are blockers, not a PASS."""
import argparse
import hashlib
import json
import tarfile
from pathlib import Path


def cdk_inventory(root):
    """Find candidate inputs only; file presence cannot prove a compatible ABI."""
    wanted = {
        "sensor_api": {"camxsensordriverapi.h"},
        "sensor_schema": {"camxsensordriver.xsd"},
        "module_schema": {"camxmoduleconfig.xsd"},
        "actuator_schema": {"camxactuatordriver.xsd"},
        "binary_compiler": {"parameterparser", "parameterparsergcc7", "parameterparser.exe", "parameterfileconverter.exe"},
    }
    found = {key: [] for key in wanted}
    if root is not None:
        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue
            for key, names in wanted.items():
                if path.name.lower() in names:
                    found[key].append({
                        "path": str(path.relative_to(root)),
                        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    })
    return {"root": str(root) if root else None, "candidates": found,
            "missing": [key for key, files in found.items() if not files],
            "abi_compatibility": "NOT_VERIFIED", "sample_rebuild": "NOT_RUN"}


def audit(sdk, cdk_root=None):
    kernel = sdk / "tmp/work-shared/radxa-dragon-q6a/kernel-source"
    config = sdk / "tmp/work-shared/radxa-dragon-q6a/kernel-build-artifacts/.config"
    archive = sdk / "downloads/chicdk-kodiak_1.0.25_armv8-2a.tar.gz"
    names = []
    if archive.is_file():
        with tarfile.open(archive) as tar:
            names = tar.getnames()
    assets = [name for name in names if "imx708" in name.lower()]
    digest = None
    if archive.is_file():
        checksum = hashlib.sha256()
        with archive.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                checksum.update(block)
        digest = checksum.hexdigest()
    text = config.read_text() if config.is_file() else ""
    cdk = cdk_inventory(cdk_root)
    return {
        "machine": "radxa-dragon-q6a", "connector": "CAM3/J7",
        "native_driver_present": (kernel / "drivers/media/i2c/imx708.c").is_file(),
        "native_driver_configured": "CONFIG_VIDEO_IMX708=m\n" in text,
        "camss_configured": "CONFIG_VIDEO_QCOM_CAMSS=m\n" in text,
        "chicdk_archive": str(archive),
        "chicdk_sha256": digest,
        "imx708_archive_assets": assets,
        "cdk_development_inputs": cdk,
        "sensor_core_recipe_present": (
            sdk / "apps/recipes-camera/imx708-sensor-core/imx708-sensor-core_1.0.bb"
        ).is_file(),
        "hardware_acceptance": "NOT_RUN",
        "blockers": (["No IMX708 assets in current Kodiak CHI archive"] if not assets else []) + (
            ["Missing CHI development inputs: " + ", ".join(cdk["missing"])] if cdk["missing"] else []
        ) + [
            "Matching CHI-CDK sensor build tools and Wide-module tuning not verified",
            "Q6A CAM3 CamX device tree and runtime driver compatibility not verified",
            "Actual FPC/board revision and sensor/actuator power sequence not verified",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--cdk-root", type=Path, help="Optional unpacked CHI development source to inventory")
    args = parser.parse_args()
    if args.cdk_root is not None and not args.cdk_root.is_dir():
        parser.error("--cdk-root must be an existing directory")
    report = audit(args.sdk.resolve(), args.cdk_root.resolve() if args.cdk_root else None)
    print(json.dumps(report, indent=2))
    return 1 if report["blockers"] else 0


if __name__ == "__main__":
    raise SystemExit(main())
