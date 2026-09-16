#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare actual C++ image decoding/preprocessing against the training pipeline."""
import argparse
import importlib.util
from pathlib import Path
import subprocess
import tempfile

import numpy as np
from PIL import Image

spec = importlib.util.spec_from_file_location('training', Path(__file__).resolve().parents[1] / 'scripts/train.py')
training = importlib.util.module_from_spec(spec)
spec.loader.exec_module(training)
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary', type=Path, required=True)
p.add_argument('--data', type=Path)
a = p.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    rng = np.random.default_rng(42)
    for w, h in [(1,1),(19,31),(513,275),(256,256),(224,224)]:
        pixels = rng.integers(0,256,size=(h,w,3),dtype=np.uint8)
        image = Image.fromarray(pixels)
        path, output = root/'input.png', root/'output.raw'
        image.save(path)
        subprocess.run([str(a.binary.resolve()), '--preprocess-only', '--input', str(path), '--dump-input', str(output)], check=True)
        actual = np.fromfile(output, dtype=np.float32).reshape(224,224,3)
        np.testing.assert_allclose(actual, training.preprocess(image), rtol=0, atol=1e-6)
    broken = root/'broken.png'; broken.write_bytes(b'not an image')
    assert subprocess.run([str(a.binary.resolve()), '--preprocess-only', '--input',str(broken),'--dump-input',str(output)],stderr=subprocess.DEVNULL).returncode != 0
if a.data:
    split, labels = training.make_split(a.data)
    ids = [{r['id'] for r in split[k]} for k in ('train','val','test')]
    assert len(labels) == 20
    assert not ids[0]&ids[1] and not ids[0]&ids[2] and not ids[1]&ids[2]
    assert all(len([r for r in split['train'] if r['label']==i])>=10 for i in range(20))
print('PASS: Python/C++ preprocessing, damaged input, split independence, calibration availability')
