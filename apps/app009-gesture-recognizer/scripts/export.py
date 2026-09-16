#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Export both static graphs; random temporal weights only with explicit --smoke."""

import argparse, json, hashlib
from pathlib import Path
import numpy as np
import torch
import onnxruntime as ort
from model import Features, Temporal, LABELS, ARCHITECTURES, architecture_name


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--checkpoint", type=Path)
    p.add_argument("--window", type=int, choices=[32, 48], default=32)
    p.add_argument("--smoke", action="store_true")
    p.add_argument("--architecture", choices=list(ARCHITECTURES))
    a = p.parse_args()
    if not a.checkpoint and not a.smoke:
        p.error("A trained checkpoint is required")
    torch.manual_seed(42)
    torch.set_num_threads(4)
    a.output.mkdir(parents=True, exist_ok=True)
    ck = (
        torch.load(a.checkpoint, map_location="cpu", weights_only=False)
        if a.checkpoint
        else None
    )
    architecture = architecture_name(
        ck.get("architecture", "global") if ck else (a.architecture or "global")
    )
    if ck and a.architecture and architecture != architecture_name(a.architecture):
        raise ValueError("Requested architecture differs from checkpoint")
    features = Features(pretrained=not bool(ck), architecture=architecture).eval()
    temporal = Temporal(a.window, architecture).eval()
    if ck:
        if ck["window"] != a.window or not ck.get("trained"):
            raise ValueError("Checkpoint window/training state mismatch")
        features.load_state_dict(ck["features"])
        temporal.load_state_dict(ck["temporal"])
    samples = {
        "features": torch.randn(1, 3, 224, 224),
        "temporal": torch.randn(1, 576, 1, a.window),
    }
    results = {}
    for name, model in [("features", features), ("temporal", temporal)]:
        x = samples[name]
        path = a.output / f"{name}.onnx"
        torch.onnx.export(
            model,
            x,
            path,
            opset_version=17,
            input_names=["input"],
            output_names=["output"],
            dynamo=False,
        )
        with torch.no_grad():
            y = model(x).numpy()
        z = ort.InferenceSession(str(path), providers=["CPUExecutionProvider"]).run(
            None, {"input": x.numpy()}
        )[0]
        error = float(np.max(np.abs(y - z)))
        assert np.allclose(y, z, atol=1e-4, rtol=1e-4)
        results[name] = {
            "max_abs_error": error,
            "input_shape": list(x.shape),
            "output_shape": list(y.shape),
        }
    if a.smoke:
        # Synthetic calibration is permitted only for operator smoke, never acceptance.
        (a.output / "calibration").mkdir(exist_ok=True)
        paths = {n: [] for n in samples}
        with torch.no_grad():
            for i in range(8):
                x = torch.rand(a.window, 3, 224, 224) * 2 - 1
                f = features(x)
                for name, array in [
                    ("features", x[:1].numpy().transpose(0, 2, 3, 1)),
                    ("temporal", f.numpy()[None, None, :, :]),
                ]:
                    path = (a.output / "calibration" / f"{name}{i}.raw").resolve()
                    array.astype(np.float32).tofile(path)
                    paths[name].append(str(path))
        for n, v in paths.items():
            (a.output / f"{n}-calibration.txt").write_text("\n".join(v) + "\n")
    (a.output / "labels.txt").write_text("\n".join(LABELS) + "\n")
    (a.output / "export.json").write_text(
        json.dumps(
            {
                "trained": bool(a.checkpoint),
                "smoke_only": a.smoke,
                "window": a.window,
                "architecture": architecture,
                "checkpoint_sha256": (
                    hashlib.sha256(a.checkpoint.read_bytes()).hexdigest()
                    if a.checkpoint
                    else None
                ),
                "graphs": results,
            },
            indent=2,
        )
        + "\n"
    )
    print(json.dumps(results), flush=True)


if __name__ == "__main__":
    main()
