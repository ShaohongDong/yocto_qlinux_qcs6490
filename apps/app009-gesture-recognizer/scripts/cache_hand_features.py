#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compute float reference features from original videos, not JPEG training caches."""

import argparse, json, time, hashlib, shutil
from pathlib import Path
import cv2, numpy as np, torch
from common import normalize, resize_rgb
from hand_frontend import sample_rect
from hand_model import Appearance
from model import Features


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--cache", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--wait", action="store_true")
    a = p.parse_args()
    a.output.mkdir(exist_ok=True)
    torch.set_num_threads(2)
    cv2.setNumThreads(1)
    torch.manual_seed(42)
    roi = Appearance().cuda().eval()
    full = Features(True, "spatial").cuda().eval()
    torch.save(roi.state_dict(), a.output / "pretrained-roi.pt")
    torch.save(full.state_dict(), a.output / "pretrained-full.pt")
    meta = json.loads((a.data / "split.json").read_text())
    pending = {n: v for n, v in meta["videos"].items() if v["split"] != "test"}
    start = time.monotonic()
    while pending:
        ready = [n for n in pending if (a.cache / n / "complete.json").exists()]
        if not ready:
            if not a.wait:
                raise RuntimeError("Frontend cache incomplete")
            if time.monotonic() - start > 3 * 3600:
                raise TimeoutError("Frontend preparation exceeded budget")
            time.sleep(5)
            continue
        for name in ready:
            v = pending.pop(name)
            out = a.cache / name
            done = out / "features-complete.json"
            if done.exists():
                continue
            if shutil.disk_usage(a.cache).free < 5 * 1024**3:
                raise RuntimeError("Disk reserve reached")
            ids = np.load(out / "frames.npy")
            rects = np.load(out / "rects.npy")
            valid = np.load(out / "valid.npy")
            cap = cv2.VideoCapture(str(a.data / v["path"]))
            batch_roi = []
            batch_full = []
            ys = []
            zs = []
            count = 0
            i = 0

            def flush():
                if not batch_roi:
                    return
                with torch.inference_mode():
                    for images, model, dest in [
                        (batch_roi, roi, ys),
                        (batch_full, full, zs),
                    ]:
                        dest.append(
                            model(
                                torch.from_numpy(
                                    normalize(np.stack(images))
                                    .transpose(0, 3, 1, 2)
                                    .copy()
                                ).cuda()
                            )
                            .cpu()
                            .numpy()
                            .astype(np.float16)
                        )
                batch_roi.clear()
                batch_full.clear()

            while True:
                ok, bgr = cap.read()
                if not ok:
                    break
                frame = count
                count += 1
                if i >= len(ids) or frame != ids[i]:
                    continue
                rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
                batch_full.append(resize_rgb(rgb))
                batch_roi.append(
                    sample_rect(rgb, rects[i], 224)
                    if valid[i]
                    else np.zeros((224, 224, 3), np.uint8)
                )
                i += 1
                if len(batch_roi) == 64:
                    flush()
            flush()
            cap.release()
            assert count == v["frames"] and i == len(ids)
            y = np.concatenate(ys)
            y[~valid] = 0
            np.save(out / "roi.npy", y)
            np.save(out / "absolute.npy", np.concatenate(zs))
            done.write_text(
                json.dumps(
                    {
                        "samples": i,
                        "source": "Original decoded RGB; training JPEG is augmentation only",
                        "frontend_fingerprint": json.loads(
                            (out / "complete.json").read_text()
                        )["fingerprint"],
                    }
                )
                + "\n"
            )
            print(name, i, flush=True)
    (a.output / "features-complete.json").write_text(
        json.dumps({"seconds": time.monotonic() - start, "videos": 160}) + "\n"
    )


if __name__ == "__main__":
    main()
