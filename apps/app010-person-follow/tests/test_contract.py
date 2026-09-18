#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check deployed C++ numerical preprocessing/decoding against calibration/reference."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import numpy as np
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
from pose_common import preprocess, decode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--imgsz', type=int, choices=(416, 640), default=416)
    args = parser.parse_args()
    rng = np.random.default_rng(42)
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        for width, height in [(1, 1), (7, 5), (333, 777), (1280, 720), (959, 539)]:
            rgb = rng.integers(0, 256, (height, width, 3), np.uint8)
            rgb.tofile(root / 'rgb')
            subprocess.run([str(args.probe.resolve()), 'preprocess', str(width), str(height), str(root/'rgb'), str(root/'input'), str(args.imgsz)], check=True)
            cpp = np.fromfile(root/'input', np.float32).reshape(args.imgsz, args.imgsz, 3)
            np.testing.assert_allclose(cpp, preprocess(rgb, args.imgsz), atol=1e-7, rtol=0)
        raw = np.zeros((56, sum((args.imgsz // s) ** 2 for s in (8, 16, 32))), np.float32)
        for i, x in enumerate([100, 101, 300]):
            raw[:5, i] = [x, 208, 80, 180, .9-i*.1]
            raw[5:, i] = np.tile([x, 208, .8], 17)
        raw[5, 0] = np.nan
        raw.tofile(root/'raw')
        subprocess.run([str(args.probe.resolve()), 'decode', '1280', '720', str(root/'raw'), str(root/'poses'), str(args.imgsz)], check=True)
        actual = np.loadtxt(root/'poses', ndmin=2)
        expected = np.array([[b['score'], *b['box'], *np.array(b['keypoints']).flatten()] for b in decode(raw,1280,720,side=args.imgsz)])
        np.testing.assert_allclose(actual, expected, atol=.0001, rtol=0)
    print('Python/C++ preprocessing and pose decode contract PASS')


if __name__ == '__main__':
    main()
