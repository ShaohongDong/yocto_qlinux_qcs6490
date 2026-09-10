#!/usr/bin/env python3
"""Shared offline structural audit helpers for Radxa Dragon Yocto images."""

import argparse
import hashlib
import json
import os
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
FDT_MAGIC = 0xD00DFEED
FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command):
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    return subprocess.run(
        command,
        check=True,
        text=True,
        capture_output=True,
        env=environment,
    ).stdout.strip()


def find_tool(name):
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"required tool is unavailable: {name}")
    return path


def dtb_root_stringlist_property(path, property_name):
    """Read a root-node string-list property without requiring libfdt tools."""
    data = path.read_bytes()
    require(len(data) >= 40, f"{path.name}: truncated FDT header")
    (
        magic,
        total_size,
        structure_offset,
        strings_offset,
        _reservation_offset,
        version,
        _last_compatible_version,
        _boot_cpuid,
        strings_size,
        structure_size,
    ) = struct.unpack_from(">10I", data)
    require(magic == FDT_MAGIC, f"{path.name}: invalid FDT magic")
    require(version >= 17, f"{path.name}: unsupported FDT version {version}")
    require(total_size <= len(data), f"{path.name}: truncated FDT data")
    require(
        structure_offset + structure_size <= total_size
        and strings_offset + strings_size <= total_size,
        f"{path.name}: invalid FDT block bounds",
    )

    structure_end = structure_offset + structure_size
    strings_end = strings_offset + strings_size
    offset = structure_offset
    depth = -1
    while offset + 4 <= structure_end:
        token = struct.unpack_from(">I", data, offset)[0]
        offset += 4
        if token == FDT_BEGIN_NODE:
            nul = data.find(b"\0", offset, structure_end)
            require(nul >= 0, f"{path.name}: unterminated FDT node name")
            depth += 1
            offset = (nul + 4) & ~3
        elif token == FDT_END_NODE:
            depth -= 1
        elif token == FDT_PROP:
            require(offset + 8 <= structure_end, f"{path.name}: truncated FDT property")
            length, name_offset = struct.unpack_from(">II", data, offset)
            offset += 8
            require(offset + length <= structure_end, f"{path.name}: truncated FDT value")
            require(name_offset < strings_size, f"{path.name}: invalid FDT property name")
            name_start = strings_offset + name_offset
            name_end = data.find(b"\0", name_start, strings_end)
            require(name_end >= 0, f"{path.name}: unterminated FDT property name")
            name = data[name_start:name_end].decode("ascii")
            value = data[offset : offset + length]
            offset = (offset + length + 3) & ~3
            if depth == 0 and name == property_name:
                require(value.endswith(b"\0"), f"{path.name}: malformed {property_name}")
                return tuple(
                    item.decode("utf-8") for item in value.rstrip(b"\0").split(b"\0")
                )
        elif token == FDT_NOP:
            continue
        elif token == FDT_END:
            break
        else:
            raise RuntimeError(f"{path.name}: unknown FDT token {token}")
    raise RuntimeError(f"{path.name}: root property is missing: {property_name}")


def pe_section(path, section_name):
    """Extract the unpadded contents of a named PE/COFF section."""
    data = path.read_bytes()
    require(len(data) >= 64 and data[:2] == b"MZ", f"{path.name}: invalid DOS header")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    require(
        pe_offset + 24 <= len(data) and data[pe_offset : pe_offset + 4] == b"PE\0\0",
        f"{path.name}: invalid PE header",
    )
    coff_offset = pe_offset + 4
    section_count = struct.unpack_from("<H", data, coff_offset + 2)[0]
    optional_size = struct.unpack_from("<H", data, coff_offset + 16)[0]
    section_offset = coff_offset + 20 + optional_size
    require(
        section_offset + section_count * 40 <= len(data),
        f"{path.name}: truncated PE section table",
    )
    expected_name = section_name.encode("ascii")
    for index in range(section_count):
        header = section_offset + index * 40
        name = data[header : header + 8].rstrip(b"\0")
        if name != expected_name:
            continue
        virtual_size, raw_size, raw_offset = struct.unpack_from("<I4xII", data, header + 8)
        require(virtual_size <= raw_size, f"{path.name}: invalid {section_name} size")
        require(
            raw_offset + raw_size <= len(data),
            f"{path.name}: truncated {section_name} section",
        )
        return data[raw_offset : raw_offset + virtual_size]
    raise RuntimeError(f"{path.name}: PE section is missing: {section_name}")


def gpt_partitions(path, sector_size):
    with path.open("rb") as image:
        image.seek(sector_size)
        header = bytearray(image.read(sector_size))
        require(
            header[:8] == b"EFI PART",
            f"{path.name}: missing {sector_size}-byte GPT",
        )
        header_size = struct.unpack_from("<I", header, 12)[0]
        expected_crc = struct.unpack_from("<I", header, 16)[0]
        require(92 <= header_size <= sector_size, f"{path.name}: invalid GPT header size")
        checked = header[:header_size]
        struct.pack_into("<I", checked, 16, 0)
        require(
            (zlib.crc32(checked) & 0xFFFFFFFF) == expected_crc,
            f"{path.name}: invalid GPT header CRC",
        )
        entries_lba, count, entry_size, entries_crc = struct.unpack_from(
            "<QIII", header, 72
        )
        require(entry_size >= 128 and count > 0, f"{path.name}: invalid GPT entry table")
        image.seek(entries_lba * sector_size)
        entries = image.read(count * entry_size)
        require(
            (zlib.crc32(entries) & 0xFFFFFFFF) == entries_crc,
            f"{path.name}: invalid GPT entry CRC",
        )

    result = []
    for offset in range(0, len(entries), entry_size):
        entry = entries[offset : offset + entry_size]
        if entry[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        result.append(
            {
                "type": str(uuid.UUID(bytes_le=entry[:16])).upper(),
                "first_lba": first,
                "last_lba": last,
                "name": entry[56:128].decode("utf-16-le").rstrip("\0"),
            }
        )
    return result


def audit_bmap(image, bmap):
    require(bmap.is_file(), f"missing block map: {bmap}")
    root = ET.parse(bmap).getroot()
    values = {
        child.tag.rsplit("}", 1)[-1]: (child.text or "").strip() for child in root
    }
    require(
        int(values["ImageSize"]) == image.stat().st_size,
        f"{bmap.name}: ImageSize does not match image",
    )
    require(int(values["BlockSize"]) > 0, f"{bmap.name}: invalid BlockSize")


def audit_image(path, bmap, sector_size):
    partitions = gpt_partitions(path, sector_size)
    require(len(partitions) == 2, f"{path.name}: expected exactly ESP and rootfs")
    require(
        partitions[0]["type"] == ESP_GUID and partitions[0]["name"] == "ESP",
        f"{path.name}: invalid ESP partition",
    )
    require(
        partitions[1]["type"] == LINUX_GUID
        and partitions[1]["name"] == "rootfs",
        f"{path.name}: invalid rootfs partition",
    )
    require(
        partitions[0]["last_lba"] < partitions[1]["first_lba"],
        f"{path.name}: overlapping partitions",
    )
    audit_bmap(path, bmap)
    return partitions


def audit_bios(bios, required):
    require(bios.is_dir(), f"missing BIOS directory: {bios}")
    for name in required:
        path = bios / name
        require(path.is_file() and path.stat().st_size, f"BIOS is missing {name}")

    for xml_name in ("rawprogram0.xml", "patch0.xml"):
        for element in ET.parse(bios / xml_name).getroot().iter():
            filename = element.attrib.get("filename", "")
            if filename and filename != "DISK":
                require(
                    (bios / filename).is_file(),
                    f"{xml_name} references missing {filename}",
                )


def audit_uki(uki, dtb, embedded, board_name):
    original_sha256 = sha256(uki)
    embedded.write_bytes(pe_section(uki, ".dtb"))
    require(sha256(uki) == original_sha256, f"{uki.name}: UKI changed during audit")
    require(
        embedded.is_file() and sha256(embedded) == sha256(dtb),
        f"{uki.name}: embedded DTB differs from deployed {board_name} DTB",
    )
    return original_sha256


def extract_esp_uki(image, esp, sector_size, destination, machine):
    offset = esp["first_lba"] * sector_size
    run(
        [
            find_tool("mcopy"),
            "-i",
            f"{image}@@{offset}",
            f"::EFI/Linux/linux-{machine}.efi",
            str(destination),
        ]
    )
    require(
        destination.is_file() and destination.stat().st_size,
        f"{image.name}: missing {machine} UKI in ESP",
    )


def audit_ext4_paths(image, paths):
    if not paths:
        return
    require(image.is_file(), f"missing root filesystem image: {image}")
    debugfs = find_tool("debugfs")
    environment = os.environ.copy()
    environment["LC_ALL"] = "C"
    for item in paths:
        completed = subprocess.run(
            [debugfs, "-R", f"stat {item}", str(image)],
            text=True,
            capture_output=True,
            env=environment,
        )
        output = completed.stdout + completed.stderr
        require(
            completed.returncode == 0 and "Inode:" in output and "File not found" not in output,
            f"{image.name}: root filesystem is missing {item}",
        )


def audit_checksums(directory):
    checksum_file = directory / "SHA256SUMS"
    require(checksum_file.is_file(), f"missing checksum manifest: {checksum_file}")
    checked = 0
    for line in checksum_file.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        digest, name = line.split(maxsplit=1)
        name = name.lstrip(" *")
        path = directory / name
        require(path.is_file(), f"SHA256SUMS references missing {name}")
        require(sha256(path) == digest, f"checksum mismatch: {name}")
        checked += 1
    require(checked > 0, f"{checksum_file}: no checksums found")
    return checked


def audit_bundle(profile, deploy, sd, ufs):
    bundle = deploy / f"{profile['machine']}-flash-bundle-{profile['bios_version']}"
    if not bundle.is_dir():
        return None

    checked = audit_checksums(bundle)
    manifest_path = bundle / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    require(manifest.get("machine") == profile["machine"], "bundle machine mismatch")
    require(
        str(manifest.get("bios_version")) == profile["bios_version"],
        "bundle BIOS version mismatch",
    )
    for key, expected in profile.get("manifest", {}).items():
        require(manifest.get(key) == expected, f"bundle {key} mismatch")

    bundle_sd = bundle / manifest["images"]["sector_512"]
    bundle_ufs = bundle / manifest["images"]["ufs_sector_4096"]
    require(sha256(bundle_sd) == sha256(sd), "bundle 512-byte image differs from deploy image")
    require(sha256(bundle_ufs) == sha256(ufs), "bundle UFS image differs from deploy image")
    return {"path": str(bundle), "checksums_verified": checked}


def audit_deploy(profile, deploy):
    deploy = deploy.resolve()
    machine = profile["machine"]
    board_name = profile["name"]
    dtb = deploy / profile["dtb"]
    uki = deploy / f"linux-{machine}.efi"
    sd_stem = f"qcom-minimal-efi-sd-image-{machine}.rootfs"
    ufs_stem = f"qcom-minimal-efi-ufs-4k-image-{machine}.rootfs"
    sd = deploy / f"{sd_stem}.wic"
    ufs = deploy / f"{ufs_stem}.wic"
    bios = (
        deploy
        / f"{machine}-bios-{profile['bios_version']}"
        / "flat_build"
        / "spinor"
        / profile["bios_board"]
    )

    for path in (dtb, uki, sd, ufs):
        require(path.is_file(), f"missing artifact: {path}")

    compatibles = dtb_root_stringlist_property(dtb, "compatible")
    require(
        all(value in compatibles for value in profile["compatibles"]),
        f"{board_name} DTB has incorrect compatible strings",
    )

    sd_partitions = audit_image(sd, Path(str(sd) + ".bmap"), 512)
    ufs_partitions = audit_image(ufs, Path(str(ufs) + ".bmap"), 4096)
    audit_ext4_paths(deploy / f"{sd_stem}.ext4", profile.get("rootfs_paths", ()))

    with tempfile.TemporaryDirectory(prefix=f"{machine}-uki-audit-") as directory_name:
        directory = Path(directory_name)
        standalone_uki_sha256 = audit_uki(
            uki, dtb, directory / "standalone.dtb", board_name
        )

        sd_uki = directory / "sd.efi"
        extract_esp_uki(sd, sd_partitions[0], 512, sd_uki, machine)
        sd_uki_sha256 = audit_uki(
            sd_uki, dtb, directory / "sd.dtb", board_name
        )

        ufs_uki = directory / "ufs.efi"
        extract_esp_uki(ufs, ufs_partitions[0], 4096, ufs_uki, machine)
        ufs_uki_sha256 = audit_uki(
            ufs_uki, dtb, directory / "ufs.dtb", board_name
        )
        require(
            sd_uki_sha256 == ufs_uki_sha256,
            "SD/NVMe and UFS images contain different UKIs",
        )

    audit_bios(bios, profile["bios_required"])
    bundle = audit_bundle(profile, deploy, sd, ufs)
    return {
        "status": "passed",
        "machine": machine,
        "dtb_sha256": sha256(dtb),
        "uki_sha256": standalone_uki_sha256,
        "rootfs_paths_verified": list(profile.get("rootfs_paths", ())),
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
        "bundle": bundle,
    }


def cli(profile):
    parser = argparse.ArgumentParser(
        description=f"Offline structural audit for {profile['name']} Yocto artifacts"
    )
    parser.add_argument("deploy_dir", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(audit_deploy(profile, args.deploy_dir), indent=2, sort_keys=True))
        return 0
    except (
        KeyError,
        OSError,
        RuntimeError,
        subprocess.CalledProcessError,
        ET.ParseError,
        ValueError,
    ) as error:
        print(f"{profile['name']} audit failed: {error}", file=sys.stderr)
        return 1
