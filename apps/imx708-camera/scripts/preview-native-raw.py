#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Unpack one captured pRAA RAW10 frame to 16-bit Bayer and half-size RGB PNG.

Requires numpy and Pillow on the analysis host. RGB combines each RGGB cell;
it applies no white balance, colour calibration, denoising or exposure scaling.
The PNG is a diagnostic preview, not ISP image-quality acceptance.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def unpack(data, width, height, stride):
    packed = np.frombuffer(data, dtype=np.uint8).reshape(height, stride)
    groups = packed[:, :width * 5 // 4].reshape(height, width // 4, 5).astype(np.uint16)
    pixels = np.empty((height, width // 4, 4), dtype=np.uint16)
    for i in range(4):
        pixels[:, :, i] = (groups[:, :, i] << 2) | ((groups[:, :, 4] >> (2 * i)) & 3)
    return pixels.reshape(height, width)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--frame", type=int, default=0)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    meta = json.loads((args.capture / "capture.json").read_text())
    if not meta["capture_complete"] or not 0 <= args.frame < meta["requested_frames"]:
        parser.error("requires a complete capture and a valid frame index")
    if "'pRAA'" not in meta["format"] or "2304/1296" not in meta["format"]:
        parser.error("only 2304x1296 packed RGGB10 is supported")
    with (args.capture / "frames.raw").open("rb") as source:
        source.seek(args.frame * meta["sizeimage"])
        data = source.read(meta["stride"] * 1296)
    raw = unpack(data, 2304, 1296, meta["stride"])
    rgb = np.stack((raw[0::2, 0::2],
                    (raw[0::2, 1::2] + raw[1::2, 0::2]) // 2,
                    raw[1::2, 1::2]), axis=-1)
    args.output.mkdir(parents=True, exist_ok=False)
    Image.fromarray(raw).save(args.output / "bayer10.png")
    Image.fromarray((rgb >> 2).astype(np.uint8)).save(args.output / "rgb.png")
    print(json.dumps({"min": int(raw.min()), "max": int(raw.max()),
                      "mean": float(raw.mean()), "std": float(raw.std())}))


if __name__ == "__main__":
    main()
