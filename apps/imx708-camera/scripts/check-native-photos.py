#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate native JPEG/RAW/JSON photo groups using an independent numpy reference.

Requires numpy and Pillow. Verifies file consistency and rendering, not sensor
calibration, colour accuracy, autofocus or exposure quality.
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image


def validate(jpeg):
    metadata = json.loads(jpeg.with_suffix(".json").read_text())
    if not metadata.get("capture_complete") or metadata.get("backend") != "native":
        raise ValueError("incomplete or non-native photo")
    if (metadata["format"], metadata["width"], metadata["height"],
            metadata["jpeg_width"], metadata["jpeg_height"]) != ("pRAA", 2304, 1296, 1152, 648):
        raise ValueError("unexpected photo dimensions/format")
    width, height, stride = metadata["width"], metadata["height"], metadata["stride"]
    data = jpeg.with_suffix(".raw").read_bytes()
    if stride < width * 5 // 4 or len(data) != height * stride or len(data) != metadata["bytes"]:
        raise ValueError("RAW length/stride mismatch")
    packed = np.frombuffer(data, dtype=np.uint8).reshape(height, stride)
    groups = packed[:, :width * 5 // 4].reshape(height, -1, 5).astype(np.uint16)
    raw = np.stack([(groups[:, :, i] << 2) | ((groups[:, :, 4] >> (2 * i)) & 3)
                    for i in range(4)], axis=-1).reshape(height, width)
    rgb = np.stack((raw[::2, ::2], (raw[::2, 1::2] + raw[1::2, ::2]) // 2,
                    raw[1::2, 1::2]), axis=-1).astype(np.float64)
    black = metadata["black_level"]
    gains = np.array([metadata["red_gain"], 1.0, metadata["blue_gain"]])
    if not 0 <= black < 1023 or not np.isfinite(gains).all() or (gains <= 0).any() or metadata["gamma"] != 2.2:
        raise ValueError("invalid colour settings")
    reference = np.floor(np.clip((rgb - black) / (1023 - black) * gains, 0, 1) ** (1 / 2.2) * 255 + 0.5)
    with Image.open(jpeg) as image:
        if image.format != "JPEG" or image.size != (1152, 648):
            raise ValueError("invalid JPEG")
        decoded = np.asarray(image.convert("RGB"), dtype=np.float64)
    error = float(np.abs(decoded - reference).mean())
    if error > 5:
        raise ValueError(f"JPEG does not match corresponding processed RAW: MAE={error:.3f}")
    return {"jpeg": str(jpeg), "sequence": metadata["sequence"],
            "timestamp_monotonic_ns": metadata["timestamp_monotonic_ns"], "rgb_mae": error}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--minimum", type=int, default=3)
    args = parser.parse_args()
    photos = sorted(args.directory.glob("*.jpg"))
    if len(photos) < args.minimum:
        parser.error(f"expected at least {args.minimum} photos, found {len(photos)}")
    try:
        records = [validate(photo) for photo in photos]
        timestamps = [record["timestamp_monotonic_ns"] for record in records]
        if any(timestamp <= 0 for timestamp in timestamps) or len(set(timestamps)) != len(timestamps):
            raise ValueError("invalid or repeated photo timestamps")
        print(json.dumps(records, indent=2))
    except (KeyError, OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
