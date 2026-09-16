#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Score frozen gesture models or Q6A predictions without tuning on test data."""

import argparse, json, hashlib, time
from pathlib import Path
import numpy as np
import torch
import onnxruntime as ort
from model import Features, Temporal, architecture_name
from train import load_records, probabilities
from common import normalize
from events import Decoder, event_metrics


def score(probabilities_by_video, metadata, window, threshold, release):
    pred = {}
    truth = {}
    bg = 0.0
    conf = np.zeros((12, 12), int)
    for name, v in metadata.items():
        ps = probabilities_by_video[name]
        n = (v["frames"] + 2) // 3
        if ps.shape != (n, 12) or not np.isfinite(ps[window - 1 :]).all():
            raise ValueError("Missing or malformed predictions " + name)
        decoder = Decoder(threshold, release)
        for i in range(window - 1, n):
            decoder.update(ps[i], i / 10)
        decoder.close(n / 10, False)
        pred[name] = decoder.events
        truth[name] = [
            {
                "label": s["label"],
                "start": s["start_frame"] / 30,
                "end": s["end_frame"] / 30,
            }
            for s in v["segments"]
            if s["label"]
        ]
        bg += sum(
            (s["end_frame"] - s["start_frame"]) / 30
            for s in v["segments"]
            if not s["label"]
        )
        for s in truth[name]:
            lo = max(window - 1, int(s["start"] * 10))
            hi = min(n, int(s["end"] * 10))
            values = ps[lo:hi]
            label = int(values.mean(0).argmax()) if len(values) else 0
            conf[s["label"], label] += 1
    m = event_metrics(pred, truth, bg)
    tp = np.diag(conf)
    m.update(
        clip_top1=float(tp.sum() / max(conf.sum(), 1)),
        clip_macro_f1=float(
            (2 * tp / np.maximum(conf.sum(0) + conf.sum(1), 1))[1:].mean()
        ),
        clip_confusion=conf.tolist(),
        clip_protocol="mean causal probabilities inside each annotated action interval; pre-warmup action with no prediction counts as background miss",
        window=window,
        threshold=threshold,
        release=release,
    )
    m["accuracy_pass"] = (
        m["clip_top1"] >= 0.8
        and m["clip_macro_f1"] >= 0.75
        and m["event_macro_f1"] >= 0.7
        and m["background_false_events_per_minute"] <= 2
    )
    return m, pred


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--cache", type=Path, required=True)
    p.add_argument("--selection", type=Path, required=True)
    p.add_argument("--split", choices=["val", "test"], required=True)
    p.add_argument("--backend", choices=["torch", "onnx", "board"], required=True)
    p.add_argument("--model", type=Path)
    p.add_argument("--predictions", type=Path)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    sel = json.loads(a.selection.read_text())
    window = sel["window"]
    threshold = sel["threshold"]
    release = sel["release"]
    split = json.loads((a.data / "split.json").read_text())
    meta = {k: v for k, v in split["videos"].items() if v["split"] == a.split}
    ps = {}
    start = time.monotonic()
    a.output.mkdir(parents=True, exist_ok=True)
    if (
        sel["split_sha256"]
        != hashlib.sha256((a.data / "split.json").read_bytes()).hexdigest()
    ):
        raise ValueError("Selection split mismatch")
    if a.backend == "board":
        for name, v in meta.items():
            ps[name] = np.full(((v["frames"] + 2) // 3, 12), np.nan, np.float32)
        for line in a.predictions.read_text().splitlines():
            row = json.loads(line)
            if row["loop"] != 0:
                raise ValueError("Only first pass accepted")
            name = Path(row["input"]).stem
            if name not in ps:
                raise ValueError("Unexpected test video " + name)
            frame = row["source_frame"]
            i = frame // 3
            if (
                frame % 3
                or i < window - 1
                or i >= len(ps[name])
                or np.isfinite(ps[name][i]).any()
            ):
                raise ValueError("Wrong or duplicated sample index")
            if abs(row["pts"] - frame / 30) > 1e-3:
                raise ValueError("Timestamp mismatch")
            values = np.array(row["probabilities"])
            assert (
                values.shape == (12,)
                and (values >= 0).all()
                and abs(values.sum() - 1) < 1e-4
            )
            ps[name][i] = values
    else:
        torch.set_num_threads(4)
        if a.backend == "torch":
            if (
                hashlib.sha256(a.model.read_bytes()).hexdigest()
                != sel["checkpoint_sha256"]
            ):
                raise ValueError("Checkpoint differs from frozen selection")
            ck = torch.load(a.model, weights_only=False)
            architecture = architecture_name(ck.get("architecture", "global"))
            if architecture != architecture_name(sel.get("architecture", "global")):
                raise ValueError("Architecture differs from frozen selection")
            features = Features(False, architecture).cuda().eval()
            features.load_state_dict(ck["features"])
            temporal = Temporal(window, architecture).cuda().eval()
            temporal.load_state_dict(ck["temporal"])
        else:
            export = json.loads((a.model / "export.json").read_text())
            if export.get("checkpoint_sha256") != sel["checkpoint_sha256"]:
                raise ValueError("ONNX export differs from frozen selection")
            if architecture_name(
                export.get("architecture", "global")
            ) != architecture_name(sel.get("architecture", "global")):
                raise ValueError("ONNX architecture differs from frozen selection")
            opts = ort.SessionOptions()
            opts.intra_op_num_threads = 4
            features = ort.InferenceSession(
                str(a.model / "features.onnx"), opts, providers=["CPUExecutionProvider"]
            )
            temporal = ort.InferenceSession(
                str(a.model / "temporal.onnx"), opts, providers=["CPUExecutionProvider"]
            )
        for name in meta:
            rgb = np.load(a.cache / name / "rgb.npy", mmap_mode="r")
            vectors = []
            with torch.inference_mode():
                for i in range(0, len(rgb), 64 if a.backend == "torch" else 1):
                    x = (
                        normalize(rgb[i : i + (64 if a.backend == "torch" else 1)])
                        .transpose(0, 3, 1, 2)
                        .copy()
                    )
                    vectors.append(
                        features(torch.from_numpy(x).cuda()).cpu().numpy()
                        if a.backend == "torch"
                        else features.run(None, {"input": x})[0]
                    )
            vectors = np.concatenate(vectors)
            if a.backend == "torch":
                ps[name] = probabilities(temporal, vectors, window)
            else:
                scores = np.zeros((len(vectors), 12), np.float32)
                for i in range(window - 1, len(vectors)):
                    x = vectors[i - window + 1 : i + 1].T[None, :, None, :].copy()
                    logits = temporal.run(None, {"input": x})[0][0]
                    probs = np.exp(logits - logits.max())
                    scores[i] = probs / probs.sum()
                ps[name] = scores
            np.save(a.output / (name + ".npy"), ps[name])
            print("Evaluated", name, flush=True)
    metrics, events = score(ps, meta, window, threshold, release)
    metrics.update(
        split=a.split,
        backend=a.backend,
        videos=len(meta),
        seconds=time.monotonic() - start,
        selection_sha256=hashlib.sha256(a.selection.read_bytes()).hexdigest(),
    )
    (a.output / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    (a.output / "events.json").write_text(json.dumps(events, indent=2) + "\n")
    print(json.dumps(metrics), flush=True)


if __name__ == "__main__":
    main()
