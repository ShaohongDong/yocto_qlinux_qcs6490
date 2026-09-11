#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check actual outputs of --exercise-dir, never certify camera hardware."""
import argparse
import json
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    photos = list(args.directory.glob("*.jpg"))
    movies = list(args.directory.glob("*.mp4"))
    if len(photos) != 1 or len(movies) != 2 or list(args.directory.glob("*.partial")):
        parser.error("expected one completed JPEG, two MP4s and no partial files in a fresh directory")
    for path in photos + movies:
        data = json.loads(subprocess.check_output([
            "ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", str(path)
        ], text=True))
        streams = data["streams"]
        if len(streams) != 1 or streams[0]["codec_type"] != "video":
            parser.error(f"unexpected streams: {path}")
        stream = streams[0]
        expected = (4608, 2592, "mjpeg") if path.suffix == ".jpg" else (1920, 1080, "h264")
        if (stream["width"], stream["height"], stream["codec_name"]) != expected:
            parser.error(f"incorrect image format: {path}")
        if path.suffix == ".mp4" and (float(data["format"].get("duration", 0)) < 1 or stream["avg_frame_rate"] != "30/1"):
            parser.error(f"invalid recording duration or frame rate: {path}")
        print(f"PASS {path.name}: {expected}")
    print("Synthetic media passed; no physical camera/ISP/AF or board performance tested.")


if __name__ == "__main__":
    main()
