#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify the official archive and safely extract only CUB classes 1..20."""
import argparse
import hashlib
from pathlib import Path
import tarfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('archive', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
with a.archive.open('rb') as stream:
    digest = hashlib.file_digest(stream, 'md5').hexdigest()
if digest != '97eceeb196236b17998738112f37df78':
    raise SystemExit('Official CUB archive checksum mismatch')
a.output.mkdir(parents=True, exist_ok=True)
with tarfile.open(a.archive) as archive:
    for member in archive:
        path = Path(member.name)
        if path.is_absolute() or '..' in path.parts or member.issym() or member.islnk():
            raise SystemExit('Unsafe archive member')
        if not member.isfile() or path.parts[0] != 'CUB_200_2011':
            continue
        if len(path.parts) == 2 or (len(path.parts) == 4 and path.parts[1] == 'images' and int(path.parts[2].split('.')[0]) <= 20):
            archive.extract(member, a.output)
print(a.output / 'CUB_200_2011')
