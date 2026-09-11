#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise real embloader BLS counting in generic ARM64 UEFI, not on a board."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(*args):
    return subprocess.run([str(x) for x in args], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--loader', required=True, type=Path)
    parser.add_argument('--backend', choices=('embloader', 'systemd-boot'), default='embloader')
    parser.add_argument('--unicode-name', action='store_true', help='exercise UTF-16 filename sizing')
    parser.add_argument('--sector-size', type=int, choices=(512, 4096), default=512)
    parser.add_argument('--fat-sector-size', type=int, choices=(512, 4096))
    parser.add_argument('--firmware', default='/usr/share/qemu-efi-aarch64/QEMU_EFI.fd', type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    candidate = 'ostree-2-qcom-测试' if args.unicode_name else 'ostree-2-qcom'
    fat_sector = args.fat_sector_size or args.sector_size
    if fat_sector < args.sector_size:
        parser.error('FAT sector size must not be smaller than the device sector size')
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='qcom-ota-uefi-') as directory:
        temp = Path(directory)
        esp = temp / 'esp.img'
        with esp.open('wb') as stream:
            stream.truncate((512 if fat_sector == 4096 else 64) * 1024 * 1024)
        run('mkfs.vfat', '-F', '32', '-S', fat_sector, esp)
        run('mmd', '-i', esp, '::EFI', '::EFI/BOOT', '::loader', '::loader/entries')
        run('mcopy', '-i', esp, args.loader, '::EFI/BOOT/BOOTAA64.EFI')
        loaderconf = temp / 'loader.conf'
        loaderconf.write_text('timeout 1\ndefault ostree-*\n')
        run('mcopy', '-i', esp, loaderconf, '::loader/loader.conf')
        # Invalid kernel deliberately makes every candidate fail before Linux.
        # We assert persisted counter transitions, not kernel/board boot success.
        kernel = temp / 'Image'
        kernel.write_text('deliberately invalid kernel fixture\n')
        run('mcopy', '-i', esp, kernel, '::candidate.efi')
        run('mcopy', '-i', esp, kernel, '::fallback.efi')
        entry = temp / 'entry.conf'
        entry.write_text('title OTA candidate\nversion 2\nlinux /candidate.efi\noptions console=ttyAMA0\n')
        run('mcopy', '-i', esp, entry, f'::loader/entries/{candidate}+3.conf')
        entry.write_text('title Known good fallback fixture\nversion 1\nlinux /fallback.efi\noptions console=ttyAMA0\n')
        run('mcopy', '-i', esp, entry, '::loader/entries/ostree-1-qcom.conf')
        for attempt in range(1, 5):
            log = args.output / f'attempt-{attempt}.log'
            with log.open('w') as output:
                process = subprocess.Popen(['qemu-system-aarch64', '-machine', 'virt', '-cpu', 'cortex-a57',
                                            '-m', '512', '-bios', str(args.firmware), '-nographic',
                                            '-drive', f'if=none,id=ota,format=raw,file={esp}',
                                            '-device', f'virtio-blk-pci,drive=ota,logical_block_size={args.sector_size},physical_block_size={args.sector_size}',
                                            '-no-reboot'],
                                           stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
                try:
                    process.wait(timeout=35)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=10)
            entries = run('mdir', '-b', '-i', esp, '::loader/entries/*.conf')
            contents = log.read_text(errors='replace')
            if 'assertion "' in contents or 'ASSERT_EFI_ERROR' in contents:
                raise RuntimeError(f'Loader assertion during attempt {attempt}; inspect {log}')
            (args.output / f'attempt-{attempt}.entries').write_text(entries)
            expected = f'{candidate}+{max(3-attempt, 0)}-{min(attempt, 3)}.conf'
            if expected not in entries:
                raise RuntimeError(f'Attempt {attempt}: expected {expected}; inspect {log}')
            if attempt == 4 and args.backend == 'embloader' and ('skipping exhausted or invalid boot entry ostree-2-qcom' not in contents or
                                 'booting Known good fallback fixture' not in contents):
                raise RuntimeError('No evidence that the exhausted candidate was skipped and fallback selected')
            if attempt == 4 and args.backend == 'systemd-boot' and 'fallback.efi' not in contents:
                raise RuntimeError('No evidence that systemd-boot tried the fallback payload')
            print(f'attempt {attempt}: {expected}', flush=True)
        shutil.copyfile(esp, args.output / 'final-esp.img')
    print(f'PASS: generic UEFI counting and fallback, device={args.sector_size}, FAT={fat_sector}, unicode={args.unicode_name}; no board boot claim')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
