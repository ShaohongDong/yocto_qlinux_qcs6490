#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cache genuine 20 Hz hand observations; never reinterpret the legacy 10 Hz cache."""

import argparse, concurrent.futures, hashlib, json, os, shutil, time
from pathlib import Path
import cv2
import numpy as np
from hand_frontend import HandFrontend


def sample_frames(frames, source_fps=30, target_fps=20):
    ids = np.unique(
        np.ceil(
            np.arange(int(np.ceil(frames * target_fps / source_fps)))
            * source_fps
            / target_fps
            - 1e-9
        ).astype(np.int64)
    )
    return ids[ids < frames]


def work(task):
    name, meta, data, models, output, fingerprint, jpeg = task
    cv2.setNumThreads(1)
    out = Path(output) / name
    out.mkdir(exist_ok=True)
    done = out / "complete.json"
    if done.exists():
        m = json.loads(done.read_text())
        if m["fingerprint"] != fingerprint:
            raise ValueError("Cache fingerprint mismatch")
        return m
    if shutil.disk_usage(output).free < 5 * 1024**3:
        raise RuntimeError("Less than 5 GiB free; preserving old data")
    front = HandFrontend(models)
    ids = sample_frames(meta["frames"])
    ids = ids[ids < meta["frames"]]
    n = len(ids)
    geometry = np.zeros((n, 134), np.float32)
    rects = np.zeros((n, 4), np.float64)
    valid = np.zeros(n, bool)
    reset = np.zeros(n, bool)
    labels = np.zeros(n, np.int64)
    for seg in meta["segments"]:
        labels[(ids >= seg["start_frame"]) & (ids < seg["end_frame"])] = seg["label"]
    cap = cv2.VideoCapture(str(Path(data) / meta["path"]))
    count = 0
    i = 0
    offsets = [0]
    start = time.monotonic()
    with (out / "roi-jpeg.bin").open("wb") as f:
        while True:
            ok, bgr = cap.read()
            if not ok:
                break
            frame = count
            count += 1
            if i >= n or frame != ids[i]:
                continue
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            r = front.step(rgb, frame / 30)
            geometry[i] = r["geometry"]
            valid[i] = r["valid"]
            reset[i] = r["reset"]
            if r["input_rect"] is not None:
                rects[i] = r["input_rect"]
            if jpeg:
                ok, encoded = cv2.imencode(
                    ".jpg",
                    cv2.cvtColor(r["rgb"], cv2.COLOR_RGB2BGR),
                    [cv2.IMWRITE_JPEG_QUALITY, 80],
                )
                if not ok:
                    raise RuntimeError("JPEG cache encoding failed")
                f.write(encoded.tobytes())
                offsets.append(f.tell())
            i += 1
            if i % 1000 == 0 and shutil.disk_usage(output).free < 5 * 1024**3:
                raise RuntimeError("Disk reserve reached")
    cap.release()
    if count != meta["frames"] or i != n:
        raise ValueError((name, count, meta["frames"], i, n))
    for key, value in [
        ("frames", ids),
        ("geometry", geometry),
        ("rects", rects),
        ("valid", valid),
        ("reset", reset),
        ("labels", labels),
        ("offsets", np.array(offsets, np.int64)),
    ]:
        np.save(out / (key + ".npy"), value)
    result = {
        "video": name,
        "fingerprint": fingerprint,
        "samples": n,
        "valid_fraction": float(valid.mean()),
        "seconds": time.monotonic() - start,
        "split": meta["split"],
        "jpeg_training_only": jpeg,
    }
    done.write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--models", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--workers", type=int, default=6)
    p.add_argument("--split", choices=["train-val", "test"], default="train-val")
    a = p.parse_args()
    a.output.mkdir(exist_ok=True)
    raw = (a.data / "split.json").read_bytes()
    split = json.loads(raw)
    sources = json.loads((a.models / "sources.json").read_text())
    identity = {
        "split_sha256": hashlib.sha256(raw).hexdigest(),
        "models": sources,
        "sample_hz": 20,
        "source_fps": 30,
        "frontend_sha256": hashlib.sha256(
            (Path(__file__).parent / "hand_frontend.py").read_bytes()
        ).hexdigest(),
    }
    fingerprint = hashlib.sha256(
        json.dumps(identity, sort_keys=True).encode()
    ).hexdigest()
    (a.output / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    tasks = [
        (
            n,
            v,
            str(a.data),
            str(a.models),
            str(a.output),
            fingerprint,
            v["split"] == "train",
        )
        for n, v in split["videos"].items()
        if (v["split"] == "test") == (a.split == "test")
    ]
    start = time.monotonic()
    results = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=a.workers) as pool:
        for m in pool.map(work, tasks):
            results.append(m)
            print(json.dumps(m), flush=True)
            size = sum(f.stat().st_size for f in a.output.rglob("*") if f.is_file())
            if size > 8 * 1024**3:
                raise RuntimeError("New cache exceeds 8 GiB budget")
    (a.output / (a.split + "-complete.json")).write_text(
        json.dumps(
            {
                "fingerprint": fingerprint,
                "videos": len(results),
                "samples": sum(r["samples"] for r in results),
                "seconds": time.monotonic() - start,
                "records": results,
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
