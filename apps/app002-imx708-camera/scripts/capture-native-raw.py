#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture native Q6A CAM3 RAW10. Requires the staged diagnostic DT and drivers.

Run on the board. Output must be a new directory. The JSON and ioctl log record
the negotiated format; successful completion alone is not image validation.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import time


def validate_buffers(output, count, sizeimage):
    """Require full buffers with consecutive sequences and monotonic timestamps."""
    lines = [line for line in output.splitlines() if line.startswith("cap dqbuf:")]
    records = []
    for line in lines:
        match = re.search(r"seq:\s*(\d+) bytesused:\s*(\d+) ts:\s*([\d.]+)", line)
        if not match or "error" in line.lower() or "ts-monotonic" not in line:
            raise RuntimeError(f"invalid capture buffer: {line}")
        seq, used, timestamp = match.groups()
        records.append({"sequence": int(seq), "bytesused": int(used),
                        "timestamp": float(timestamp)})
    if len(records) != count or any(r["bytesused"] != sizeimage for r in records):
        raise RuntimeError("incomplete capture buffers")
    for previous, current in zip(records, records[1:]):
        if (current["sequence"] != previous["sequence"] + 1 or
                current["timestamp"] <= previous["timestamp"]):
            raise RuntimeError("discontinuous frame sequence or timestamp")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--frames", type=int, default=100)
    parser.add_argument("--test-pattern", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    if not 1 <= args.frames <= 100:
        parser.error("frames must be between 1 and 100")
    args.output.mkdir(parents=True, exist_ok=False)
    with (args.output / "commands.log").open("w") as log:
        def run(*command):
            log.write(repr(command) + "\n")
            log.flush()
            try:
                result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, timeout=60)
            except subprocess.TimeoutExpired as error:
                output = error.stdout or b""
                log.write(output.decode(errors="replace") if isinstance(output, bytes) else output)
                log.write("\nTIMEOUT: no successful completion within 60 seconds\n")
                log.flush()
                raise
            log.write(result.stdout)
            log.flush()
            result.check_returncode()
            return result.stdout.strip()

        candidates = []
        for media in sorted(Path("/dev").glob("media*")):
            topology = run("media-ctl", "-d", str(media), "-p")
            if "imx708" in topology and "msm_csiphy3" in topology:
                candidates.append(str(media))
        if len(candidates) != 1:
            raise RuntimeError("expected exactly one IMX708 CAMSS media graph")
        media = candidates[0]
        sensor = run("media-ctl", "-d", media, "-e", "imx708")
        video = run("media-ctl", "-d", media, "-e", "msm_vfe0_video0")
        run("media-ctl", "-d", media, "-l", '"msm_csiphy3":1 -> "msm_csid0":0 [1]')
        run("media-ctl", "-d", media, "-l", '"msm_csid0":1 -> "msm_vfe0_rdi0":0 [1]')
        run("v4l2-ctl", "-d", sensor, "--set-ctrl=wide_dynamic_range=0,horizontal_flip=0,vertical_flip=0")
        for entity, pad in (("imx708", 0), ("msm_csiphy3", 0),
                            ("msm_csid0", 0), ("msm_csid0", 1),
                            ("msm_vfe0_rdi0", 0)):
            run("media-ctl", "-d", media, "-V",
                f'"{entity}":{pad} [fmt:SRGGB10_1X10/2304x1296 field:none]')
        run("v4l2-ctl", "-d", sensor, f"--set-ctrl=test_pattern={args.test_pattern}")
        run("v4l2-ctl", "-d", video,
            "--set-fmt-video=width=2304,height=1296,pixelformat=pRAA")
        fmt = run("v4l2-ctl", "-d", video, "--get-fmt-video")
        if "2304/1296" not in fmt or "'pRAA'" not in fmt:
            raise RuntimeError("unexpected negotiated RAW format")
        stride = int(re.search(r"Bytes per Line\s*:\s*(\d+)", fmt).group(1))
        sizeimage = int(re.search(r"Size Image\s*:\s*(\d+)", fmt).group(1))
        (args.output / "topology.txt").write_text(run("media-ctl", "-d", media, "-p"))
        metadata = {"media": media, "sensor": sensor, "video": video,
                    "requested_frames": args.frames, "test_pattern": args.test_pattern,
                    "format": fmt, "start_monotonic": time.monotonic(),
                    "stride": stride, "sizeimage": sizeimage,
                    "capture_complete": False}
        manifest = args.output / "capture.json"
        manifest.write_text(json.dumps(metadata, indent=2) + "\n")
        raw = args.output / "frames.raw"
        try:
            output = run("stdbuf", "-oL", "-eL", "v4l2-ctl", "-d", video, "--verbose", "--stream-mmap=4",
                         f"--stream-count={args.frames}", f"--stream-to={raw}")
            metadata["buffers"] = validate_buffers(output, args.frames, sizeimage)
            if raw.stat().st_size != args.frames * sizeimage:
                raise RuntimeError("RAW byte count does not match requested complete frames")
            metadata["capture_complete"] = True
        except (subprocess.SubprocessError, RuntimeError, OSError) as error:
            metadata["error"] = str(error)
            raise
        finally:
            metadata.update(end_monotonic=time.monotonic(),
                            bytes=raw.stat().st_size if raw.exists() else 0)
            manifest.write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
