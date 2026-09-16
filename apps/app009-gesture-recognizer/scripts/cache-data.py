#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cache lossless 10 Hz RGB and frozen pretrained features; preserve frame labels."""

import argparse, json, time, shutil, hashlib
from pathlib import Path
import cv2, numpy as np, torch
from model import Features, ARCHITECTURES, architecture_name
from common import resize_rgb, normalize


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--architecture", choices=list(ARCHITECTURES), default="regularized")
    a = p.parse_args()
    architecture = architecture_name(a.architecture)
    a.output.mkdir(parents=True, exist_ok=True)
    split = json.loads((a.data / "split.json").read_text())
    split_hash = hashlib.sha256((a.data / "split.json").read_bytes()).hexdigest()
    existing = a.output / "complete.json"
    if existing.exists():
        previous = json.loads(existing.read_text())
        if (
            previous.get("architecture", ARCHITECTURES["global"]) != architecture
            or previous.get("split_sha256", split_hash) != split_hash
        ):
            raise RuntimeError("Existing cache uses a different architecture or split")
    torch.set_num_threads(4)
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA required")
    model = Features(architecture=architecture).cuda().eval()
    torch.save(model.state_dict(), a.output / "pretrained-features.pt")
    start = time.monotonic()
    for j, (name, v) in enumerate(sorted(split["videos"].items())):
        dest = a.output / name
        dest.mkdir(exist_ok=True)
        if (dest / "complete.json").exists():
            previous = json.loads((dest / "complete.json").read_text())
            if (
                previous.get("architecture", ARCHITECTURES["global"]) != architecture
                or previous["source_sha256"] != v["sha256"]
            ):
                raise RuntimeError("Cached record identity mismatch: " + name)
            continue
        if shutil.disk_usage(a.output).free < 5 * 1024**3:
            raise RuntimeError("Cache disk space below 5 GiB")
        n = (v["frames"] + 2) // 3
        rgb = np.lib.format.open_memmap(
            dest / "rgb.npy", mode="w+", dtype="uint8", shape=(n, 224, 224, 3)
        )
        labels = np.zeros(n, dtype=np.int64)
        for seg in v["segments"]:
            indices = np.arange(n) * 3
            labels[(indices >= seg["start_frame"]) & (indices < seg["end_frame"])] = (
                seg["label"]
            )
        cap = cv2.VideoCapture(str(a.data / v["path"]))
        i = 0
        k = 0
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            if i % 3 == 0:
                rgb[k] = resize_rgb(cv2.cvtColor(frame, cv2.COLOR_BGR2RGB))
                k += 1
            i += 1
        cap.release()
        assert i == v["frames"] and k == n, (name, i, k)
        rgb.flush()
        features = np.lib.format.open_memmap(
            dest / "features.npy", mode="w+", dtype="float32", shape=(n, 576)
        )
        prefix = np.lib.format.open_memmap(
            dest / "prefix.npy",
            mode="w+",
            dtype="float32",
            shape=(n, *model.prefix_shape),
        )
        with torch.inference_mode():
            for begin in range(0, n, 64):
                x = torch.from_numpy(
                    normalize(rgb[begin : begin + 64]).transpose(0, 3, 1, 2).copy()
                ).cuda()
                middle = model.prefix(x)
                assert tuple(middle.shape[1:]) == model.prefix_shape
                result = model.tail(middle)
                if j == 0 and begin == 0:
                    full = model(x)
                    torch.testing.assert_close(result, full, atol=1e-6, rtol=1e-6)
                    (a.output / "prefix-equivalence.json").write_text(
                        json.dumps(
                            {
                                "status": "PASS",
                                "max_abs_error": float((result - full).abs().max()),
                            }
                        )
                        + "\n"
                    )
                prefix[begin : begin + len(x)] = middle.cpu().numpy()
                features[begin : begin + len(x)] = result.cpu().numpy()
        features.flush()
        prefix.flush()
        np.save(dest / "labels.npy", labels)
        (dest / "complete.json").write_text(
            json.dumps(
                {
                    "samples": n,
                    "architecture": architecture,
                    "source_sha256": v["sha256"],
                    "sampling": "source frames 0,3,6,... at 30 FPS",
                    "features": "ImageNet MobileNetV3 small frozen prefix",
                }
            )
            + "\n"
        )
        print(j + 1, name, n, round(time.monotonic() - start, 1), flush=True)
    (a.output / "complete.json").write_text(
        json.dumps(
            {
                "videos": 200,
                "architecture": architecture,
                "split_sha256": split_hash,
                "seconds": time.monotonic() - start,
            }
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
