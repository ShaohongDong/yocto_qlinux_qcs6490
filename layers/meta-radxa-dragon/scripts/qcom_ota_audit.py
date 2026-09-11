# SPDX-License-Identifier: MIT
"""Inspect actual OTA filesystems and their WIC payloads without mounting them."""
import json
from pathlib import Path
import re
import tempfile

from radxa_dragon_audit import (audit_image, find_tool, parse_bls, require, run, sha256,
                                audit_uki, audit_module_dependencies, kernel_release)


def ext4_cat(image, path):
    require(re.fullmatch(r'/[A-Za-z0-9_./+-]+', path) is not None and '..' not in Path(path).parts,
            'unsafe path in OTA image')
    return run([find_tool('debugfs'), '-R', f'cat {path}', str(image)])


def compare_partition(disk, partition, sector_size, payload):
    require(payload.is_file(), f'missing OTA payload {payload}')
    require(payload.stat().st_size <= (partition['last_lba'] - partition['first_lba'] + 1) * sector_size,
            'OTA payload exceeds its WIC partition')
    with disk.open('rb') as source, payload.open('rb') as expected:
        source.seek(partition['first_lba'] * sector_size)
        while block := expected.read(1024 * 1024):
            require(source.read(len(block)) == block, f'{disk.name}: partition differs from {payload.name}')


def audit_ota_image(deploy, stem, machine, sectors, profile=None):
    disk = deploy / f'{stem}.wic'
    partitions = audit_image(disk, Path(str(disk) + '.bmap'), sectors)
    esp, root = deploy / f'{stem}.ota-esp', deploy / f'{stem}.ota-ext4'
    compare_partition(disk, partitions[0], sectors, esp)
    compare_partition(disk, partitions[1], sectors, root)
    manifest = json.loads((deploy / f'{stem}.ota.json').read_text())
    checksum = manifest['commit']
    require(re.fullmatch('[0-9a-f]{64}', checksum), 'invalid commit checksum')
    require(manifest['machine'] == machine, 'OTA machine mismatch')
    base = f'/ostree/deploy/qcom/deploy/{checksum}.0'
    embedded = json.loads(ext4_cat(root, base + '/usr/share/qcom-ota/identity.json'))
    require(embedded == {k: v for k, v in manifest.items() if k != 'commit'}, 'OTA identity differs from committed deployment')
    require(embedded['ref'] == f'{machine}/{embedded["image"]}/{embedded["channel"]}', 'OTA ref isolation mismatch')
    header = run([find_tool('dumpe2fs'), '-h', str(root)])
    stats = dict(line.split(':', 1) for line in header.splitlines() if ':' in line)
    available = (int(stats['Free blocks']) - int(stats['Reserved block count'])) * int(stats['Block size'])
    required = 2 * embedded['installed_bytes'] + 1024 * embedded['min_free_kib']
    require(available >= required, 'factory OTA filesystem lacks space for its first full update')
    require('LABEL=otaboot /boot vfat' in ext4_cat(root, base + '/etc/fstab'), 'missing OTA ESP mount')
    for path in ('/usr/bin/qcom-ota', '/usr/lib/systemd/system/qcom-ota-confirm.service'):
        require(ext4_cat(root, base + path), f'OTA runtime missing {path}')
    enabled = run([find_tool('debugfs'), '-R',
                   f'stat {base}/etc/systemd/system/multi-user.target.wants/qcom-ota-confirm.service', str(root)])
    require('Type: symlink' in enabled and 'qcom-ota-confirm.service' in enabled,
            'OTA confirmation service is not enabled')
    gate = ext4_cat(root, base + '/usr/lib/systemd/system/systemd-bless-boot.service.d/qcom-ota.conf')
    require('ConditionPathExists=/run/qcom-ota-health-passed' in gate,
            'upstream boot confirmation is not gated by OTA health checks')
    fat = str(esp)
    config = run([find_tool('mtype'), '-i', fat, '::/loader/loader.conf'])
    require('default ostree-*' in config, 'OTA boot selection missing')
    entries = run([find_tool('mdir'), '-b', '-i', fat, '::/loader/entries/*.conf']).splitlines()
    require(len(entries) == 1, 'factory image must have exactly one known-good deployment')
    require('+' not in Path(entries[0]).name, 'factory fallback must already be marked good')
    entry = parse_bls(run([find_tool('mtype'), '-i', fat, entries[0]]))
    require('root=LABEL=otaroot' in entry.get('options', ''), 'OTA root argument missing')
    require('ostree=' in entry.get('options', ''), 'OSTree deployment argument missing')
    for key in ('linux', 'initrd', 'devicetree', 'uki'):
        if key in entry:
            path = entry[key]
            require(re.fullmatch(r'/[A-Za-z0-9_./+-]+', path) and '..' not in Path(path).parts,
                    'invalid boot payload path')
    with tempfile.TemporaryDirectory(prefix='qcom-ota-audit-') as directory:
        temp = Path(directory)
        for key in ('linux', 'initrd', 'devicetree', 'uki'):
            if key not in entry:
                continue
            destination = temp / key
            run([find_tool('mcopy'), '-i', fat, '::' + entry[key], str(destination)])
            require(destination.stat().st_size > 0, 'empty OTA boot payload')
        if manifest['backend'] == 'embloader':
            require({'linux', 'initrd', 'devicetree'} <= entry.keys(), 'Q6A raw boot payload incomplete')
            require(profile is not None, 'embloader audit requires board profile')
            require(sha256(temp / 'devicetree') == sha256(deploy / profile['dtb']), 'Q6A OTA DTB mismatch')
            require(sha256(temp / 'linux') == sha256(deploy / 'Image'), 'Q6A OTA kernel mismatch')
            require(sha256(temp / 'initrd') == sha256(deploy / f'initramfs-ostree-image-{machine}.cpio.gz'),
                    'Q6A OTA initramfs mismatch')
            run([find_tool('mcopy'), '-i', fat, '::/EFI/BOOT/BOOTAA64.EFI', str(temp / 'loader')])
            require(sha256(temp / 'loader') == sha256(deploy / 'embloader-0.7.efi'), 'OTA loader mismatch')
            release = kernel_release(deploy, machine)
            audit_module_dependencies(root, release, base)
        elif profile is not None:
            require('uki' in entry, 'EFI OTA entry must reference its versioned UKI')
            audit_uki(temp / 'uki', deploy / profile['dtb'], temp / 'uki.dtb', profile['name'])
    return dict(status='passed', machine=machine, version=manifest['version'], commit=checksum,
                backend=manifest['backend'], sector_size=sectors, partitions=partitions,
                available_bytes=available, required_update_bytes=required,
                image_sha256=sha256(disk), evidence='offline-structure')
