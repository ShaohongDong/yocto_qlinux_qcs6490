#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Select training-only calibration images/windows and consume real HTP features."""

import argparse, json, hashlib
from pathlib import Path
import numpy as np
from common import normalize


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--cache", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--window", type=int, choices=[32, 48], required=True)
    p.add_argument("--htp-features", type=Path)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    manifest = a.output / "calibration-selection.json"
    split = json.loads((a.data / "split.json").read_text())
    if a.htp_features:
        selected = json.loads(manifest.read_text())
        paths = []
        window = a.window
        assert selected["window"] == window
        for i, row in enumerate(selected["windows"]):
            values = []
            for sample in row["input_indices"]:
                v = np.fromfile(a.htp_features / f"{sample}.raw", np.float32)
                if v.shape != (576,) or not np.isfinite(v).all():
                    raise ValueError("Bad HTP feature " + str(sample))
                values.append(v)
            path = (a.output / "calibration" / f"temporal{i}.raw").resolve()
            np.array(values, dtype=np.float32).tofile(path)
            paths.append(str(path))
        (a.output / "temporal-calibration.txt").write_text("\n".join(paths) + "\n")
        print(
            "Temporal calibration uses real quantized HTP feature outputs", flush=True
        )
        return
    if manifest.exists():
        raise RuntimeError("Calibration selection already frozen")
    pools = [[] for _ in range(12)]
    for name, v in split["videos"].items():
        if v["split"] != "train":
            continue
        labels = np.load(a.cache / name / "labels.npy")
        for end in range(a.window - 1, len(labels)):
            pools[int(labels[end])].append((name, end))
    rng = np.random.default_rng(42)
    windows = []
    inputs = []
    lookup = {}
    (a.output / "calibration").mkdir(exist_ok=True)
    (a.output / "feature-inputs").mkdir(exist_ok=True)
    for label, pool in enumerate(pools):
        for j in rng.choice(len(pool), 8, replace=False):
            name, end = pool[j]
            indices = []
            rgb = np.load(a.cache / name / "rgb.npy", mmap_mode="r")
            for i in range(end - a.window + 1, end + 1):
                key = (name, i)
                if key not in lookup:
                    idx = len(inputs)
                    lookup[key] = idx
                    path = a.output / "feature-inputs" / f"{idx}.raw"
                    normalize(rgb[i]).tofile(path)
                    inputs.append(
                        {
                            "video": name,
                            "sample": i,
                            "path": str(path.resolve()),
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                        }
                    )
                indices.append(lookup[key])
            windows.append(
                {
                    "video": name,
                    "end_sample": end,
                    "label": label,
                    "input_indices": indices,
                }
            )
    # Feature ranges include all classes and subjects selected above.
    selected_indices = sorted(
        set(
            int(x)
            for x in rng.choice(len(inputs), min(192, len(inputs)), replace=False)
        )
    )
    (a.output / "features-calibration.txt").write_text(
        "\n".join(inputs[i]["path"] for i in selected_indices) + "\n"
    )
    (a.output / "feature-inputs/list.txt").write_text(
        "\n".join(f"{i}.raw" for i in range(len(inputs))) + "\n"
    )
    manifest.write_text(
        json.dumps(
            {
                "seed": 42,
                "window": a.window,
                "split_sha256": hashlib.sha256(
                    (a.data / "split.json").read_bytes()
                ).hexdigest(),
                "windows": windows,
                "inputs": inputs,
                "feature_calibration_indices": selected_indices,
                "source_split": "train",
            },
            indent=2,
        )
        + "\n"
    )
    print(
        "Training-only calibration:",
        len(inputs),
        "images,",
        len(windows),
        "windows",
        flush=True,
    )


if __name__ == "__main__":
    main()
