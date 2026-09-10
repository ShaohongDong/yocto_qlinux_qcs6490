#!/usr/bin/env python3
"""Offline structural audit for Radxa Dragon Q6A Yocto deployment artifacts."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ET
import zlib


ESP_GUID = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
LINUX_GUID = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def gpt_partitions(path, sector_size):
    with path.open("rb") as image:
        image.seek(sector_size)
        header = bytearray(image.read(sector_size))
        require(header[:8] == b"EFI PART", f"{path.name}: missing {sector_size}-byte GPT")
        header_size = struct.unpack_from("<I", header, 12)[0]
        expected_crc = struct.unpack_from("<I", header, 16)[0]
        require(92 <= header_size <= sector_size, f"{path.name}: invalid GPT header size")
        checked = header[:header_size]
        struct.pack_into("<I", checked, 16, 0)
        require((zlib.crc32(checked) & 0xFFFFFFFF) == expected_crc,
                f"{path.name}: invalid GPT header CRC")
        entries_lba, count, entry_size, entries_crc = struct.unpack_from("<QIII", header, 72)
        require(entry_size >= 128 and count > 0, f"{path.name}: invalid GPT entry table")
        image.seek(entries_lba * sector_size)
        entries = image.read(count * entry_size)
        require((zlib.crc32(entries) & 0xFFFFFFFF) == entries_crc,
                f"{path.name}: invalid GPT entry CRC")

    result = []
    for offset in range(0, len(entries), entry_size):
        entry = entries[offset:offset + entry_size]
        if entry[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        result.append({
            "type": str(uuid.UUID(bytes_le=entry[:16])).upper(),
            "first_lba": first,
            "last_lba": last,
            "name": entry[56:128].decode("utf-16-le").rstrip("\0"),
        })
    return result


def audit_bmap(image, bmap):
    root = ET.parse(bmap).getroot()
    values = {child.tag.rsplit("}", 1)[-1]: (child.text or "").strip()
              for child in root}
    require(int(values["ImageSize"]) == image.stat().st_size,
            f"{bmap.name}: ImageSize does not match image")
    require(int(values["BlockSize"]) > 0, f"{bmap.name}: invalid BlockSize")


def audit_image(path, bmap, sector_size):
    partitions = gpt_partitions(path, sector_size)
    require(len(partitions) == 2, f"{path.name}: expected exactly ESP and rootfs")
    require(partitions[0]["type"] == ESP_GUID and partitions[0]["name"] == "ESP",
            f"{path.name}: invalid ESP partition")
    require(partitions[1]["type"] == LINUX_GUID and partitions[1]["name"] == "rootfs",
            f"{path.name}: invalid rootfs partition")
    require(partitions[0]["last_lba"] < partitions[1]["first_lba"],
            f"{path.name}: overlapping partitions")
    audit_bmap(path, bmap)
    return partitions


def audit_bios(bios):
    required = {
        "prog_firehose_ddr.elf", "rawprogram0.xml", "patch0.xml",
        "gpt_main0.bin", "gpt_backup0.bin", "xbl.elf", "xbl_config.elf",
        "imagefv.elf", "PILFV.Fv",
    }
    for name in required:
        require((bios / name).is_file() and (bios / name).stat().st_size,
                f"BIOS is missing {name}")
    for xml_name in ("rawprogram0.xml", "patch0.xml"):
        for element in ET.parse(bios / xml_name).getroot().iter():
            filename = element.attrib.get("filename", "")
            if filename and filename != "DISK":
                require((bios / filename).is_file(),
                        f"{xml_name} references missing {filename}")


def run(command):
    return subprocess.run(command, check=True, text=True, capture_output=True).stdout.strip()


def find_objcopy():
    for name in ("aarch64-qcom-linux-objcopy", "llvm-objcopy", "objcopy"):
        path = shutil.which(name)
        if path:
            return path
    raise RuntimeError("no objcopy implementation is available")


def find_tool(name):
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"required tool is unavailable: {name}")
    return path


def audit_uki(uki, dtb, objcopy, embedded):
    original_sha256 = sha256(uki)
    rewritten = embedded.parent / f"{embedded.stem}.rewritten.efi"
    run([
        objcopy, "--dump-section", f".dtb={embedded}",
        str(uki), str(rewritten),
    ])
    require(sha256(uki) == original_sha256,
            f"{uki.name}: UKI changed during audit")
    require(embedded.is_file() and sha256(embedded) == sha256(dtb),
            f"{uki.name}: embedded DTB differs from deployed Q6A DTB")
    return original_sha256


def extract_esp_uki(image, esp, sector_size, destination):
    offset = esp["first_lba"] * sector_size
    run([
        find_tool("mcopy"), "-i", f"{image}@@{offset}",
        "::EFI/Linux/linux-radxa-dragon-q6a.efi", str(destination),
    ])
    require(destination.is_file() and destination.stat().st_size,
            f"{image.name}: missing Q6A UKI in ESP")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("deploy_dir", type=Path)
    args = parser.parse_args()
    deploy = args.deploy_dir.resolve()

    dtb = deploy / "qcs6490-radxa-dragon-q6a.dtb"
    uki = deploy / "linux-radxa-dragon-q6a.efi"
    sd = deploy / "qcom-minimal-efi-sd-image-radxa-dragon-q6a.rootfs.wic"
    ufs = deploy / "qcom-minimal-efi-ufs-4k-image-radxa-dragon-q6a.rootfs.wic"
    bios = deploy / "radxa-dragon-q6a-bios-260815/flat_build/spinor/dragon-q6a"
    for path in (dtb, uki, sd, ufs):
        require(path.is_file(), f"missing artifact: {path}")

    compatibles = run(["fdtget", "-t", "s", str(dtb), "/", "compatible"]).split()
    require("radxa,dragon-q6a" in compatibles and "qcom,qcm6490" in compatibles,
            "Q6A DTB has incorrect compatible strings")

    sd_partitions = audit_image(sd, Path(str(sd) + ".bmap"), 512)
    ufs_partitions = audit_image(ufs, Path(str(ufs) + ".bmap"), 4096)
    objcopy = find_objcopy()
    with tempfile.TemporaryDirectory(prefix="q6a-uki-audit-") as directory_name:
        directory = Path(directory_name)
        standalone_uki_sha256 = audit_uki(
            uki, dtb, objcopy, directory / "standalone.dtb")

        sd_uki = directory / "sd.efi"
        extract_esp_uki(sd, sd_partitions[0], 512, sd_uki)
        sd_uki_sha256 = audit_uki(sd_uki, dtb, objcopy, directory / "sd.dtb")

        ufs_uki = directory / "ufs.efi"
        extract_esp_uki(ufs, ufs_partitions[0], 4096, ufs_uki)
        ufs_uki_sha256 = audit_uki(ufs_uki, dtb, objcopy, directory / "ufs.dtb")
        require(sd_uki_sha256 == ufs_uki_sha256,
                "SD/eMMC and UFS images contain different UKIs")

    results = {
        "status": "passed",
        "machine": "radxa-dragon-q6a",
        "dtb_sha256": sha256(dtb),
        "uki_sha256": standalone_uki_sha256,
        "images": {
            "sector_512": {
                "sha256": sha256(sd),
                "uki_sha256": sd_uki_sha256,
                "partitions": sd_partitions,
            },
            "ufs_sector_4096": {
                "sha256": sha256(ufs),
                "uki_sha256": ufs_uki_sha256,
                "partitions": ufs_partitions,
            },
        },
    }
    audit_bios(bios)
    print(json.dumps(results, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyError, OSError, RuntimeError, subprocess.CalledProcessError,
            ET.ParseError, ValueError) as error:
        print(f"Q6A audit failed: {error}", file=sys.stderr)
        sys.exit(1)
