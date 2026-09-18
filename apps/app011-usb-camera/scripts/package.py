#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package a built ARM64 executable for app-only installation."""
import argparse
import hashlib
import re
import shutil
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--version', required=True)
args = parser.parse_args()
if not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9._-]*', args.version):
    parser.error('Invalid version')
header = args.binary.read_bytes()[:20]
if header[:4] != b'\x7fELF' or header[4:6] != b'\x02\x01' or header[18:20] != b'\xb7\x00':
    parser.error('Expected little-endian ARM64 ELF')
root = Path(__file__).resolve().parents[1]
args.output.mkdir(parents=True, exist_ok=False)
(args.output / 'bin').mkdir()
shutil.copy2(args.binary, args.output / 'bin/usb-camera')
for name in ('install', 'uninstall'):
    shutil.copy2(root / 'scripts' / name, args.output / name)
for name in ('usb-camera.desktop', 'usb-camera.svg'):
    shutil.copy2(root / 'data' / name, args.output / name)
for name in ('README.md', 'LICENSE'):
    shutil.copy2(root / name, args.output / name)
(args.output / 'VERSION').write_text(args.version + '\n')
files = sorted(p for p in args.output.rglob('*') if p.is_file())
(args.output / 'SHA256SUMS').write_text(''.join(
    f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(args.output)}\n' for p in files))
