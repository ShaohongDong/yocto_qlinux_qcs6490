#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Train subject-isolated causal gesture models on CUDA, selecting on validation."""

import argparse, json, random, time, copy, hashlib
from pathlib import Path
import numpy as np
import torch
from torch import nn
from model import Features, Temporal, ARCHITECTURES, architecture_name
from common import normalize
from events import Decoder, event_metrics


def probabilities(temporal, features, window):
    result = []
    with torch.inference_mode():
        for start in range(0, len(features), 256):
            ends = np.arange(start, min(start + 256, len(features)))
            idx = np.maximum(ends[:, None] - np.arange(window - 1, -1, -1), 0)
            x = (
                torch.from_numpy(np.asarray(features[idx]).transpose(0, 2, 1).copy())
                .cuda()
                .unsqueeze(2)
            )
            result.append(torch.softmax(temporal(x), 1).cpu().numpy())
    return np.concatenate(result)


def evaluate(temporal, records, window, feature_model=None):
    temporal.eval()
    pred = {}
    truth = {}
    bg = 0.0
    conf = np.zeros((12, 12), int)
    cache = {}
    for name, r in records.items():
        if feature_model is None:
            features = r["features"]
        else:
            pieces = []
            with torch.inference_mode():
                for i in range(0, len(r["prefix"]), 128):
                    pieces.append(
                        feature_model.tail(
                            torch.from_numpy(np.array(r["prefix"][i : i + 128]))
                            .float()
                            .cuda()
                        )
                        .cpu()
                        .numpy()
                    )
            features = np.concatenate(pieces)
        ps = probabilities(temporal, features, window)
        cache[name] = ps
        truth[name] = [
            {
                "label": x["label"],
                "start": x["start_frame"] / 30,
                "end": x["end_frame"] / 30,
            }
            for x in r["meta"]["segments"]
            if x["label"]
        ]
        bg += sum(
            (x["end_frame"] - x["start_frame"]) / 30
            for x in r["meta"]["segments"]
            if not x["label"]
        )
        for s in truth[name]:
            lo = max(window - 1, int(s["start"] * 10))
            hi = min(len(ps), int(s["end"] * 10))
            v = ps[lo:hi]
            prediction = int(v.mean(0).argmax()) if len(v) else 0
            conf[s["label"], prediction] += 1
    best = None
    for threshold in [0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 0.99]:
        for release in [0.3, 0.6, 0.9]:
            pred = {}
            for name, ps in cache.items():
                d = Decoder(threshold, release)
                for i in range(window - 1, len(ps)):
                    d.update(ps[i], i / 10)
                d.close(len(ps) / 10, False)
                pred[name] = d.events
            m = event_metrics(pred, truth, bg)
            m.update(threshold=threshold, release=release)
            key = (
                (
                    m["event_macro_f1"]
                    if m["background_false_events_per_minute"] <= 2
                    else -1
                ),
                m["event_macro_f1"],
            )
            if best is None or key > best[0]:
                best = (key, m)
    m = best[1]
    tp = np.diag(conf)
    m["clip_top1"] = float(tp.sum() / max(conf.sum(), 1))
    m["clip_macro_f1"] = float(
        (2 * tp / np.maximum(conf.sum(0) + conf.sum(1), 1))[1:].mean()
    )
    m["clip_confusion"] = conf.tolist()
    return m


def load_records(data, cache, subset):
    meta = json.loads((data / "split.json").read_text())
    out = {}
    for name, v in meta["videos"].items():
        if v["split"] != subset:
            continue
        p = cache / name
        if not (p / "complete.json").exists():
            raise RuntimeError("Incomplete cache " + name)
        out[name] = {
            "meta": v,
            "features": np.load(p / "features.npy", mmap_mode="r"),
            "prefix": np.load(p / "prefix.npy", mmap_mode="r"),
            "labels": np.load(p / "labels.npy"),
        }
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--cache", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--window", type=int, choices=[32, 48], default=32)
    p.add_argument("--head-epochs", type=int, default=30)
    p.add_argument("--joint-epochs", type=int, default=20)
    p.add_argument("--resume", action="store_true")
    p.add_argument("--joint-batch", type=int, choices=[1, 2], default=2)
    p.add_argument("--architecture", choices=list(ARCHITECTURES), default="regularized")
    a = p.parse_args()
    architecture = architecture_name(a.architecture)
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA required")
    torch.set_num_threads(4)
    torch.manual_seed(42)
    np.random.seed(42)
    rng = np.random.default_rng(42)
    a.output.mkdir(parents=True, exist_ok=True)
    if (a.output / "last.pt").exists() and not a.resume:
        raise RuntimeError("Existing run requires --resume or a new output directory")
    split_hash = hashlib.sha256((a.data / "split.json").read_bytes()).hexdigest()
    resume = (
        torch.load(a.output / "resume.pt", weights_only=False) if a.resume else None
    )
    if resume and (
        resume["split_sha256"] != split_hash
        or resume["window"] != a.window
        or resume.get("architecture", ARCHITECTURES["global"]) != architecture
    ):
        raise RuntimeError("Resume split/window mismatch")
    cache_info = json.loads((a.cache / "complete.json").read_text())
    cached_architecture = cache_info.get("architecture", ARCHITECTURES["global"])
    spatial_family = {ARCHITECTURES["spatial"], ARCHITECTURES["regularized"]}
    if (
        cached_architecture != architecture
        and not {cached_architecture, architecture} <= spatial_family
    ):
        raise RuntimeError("Feature cache does not match the requested architecture")
    if cache_info.get("split_sha256", split_hash) != split_hash:
        raise RuntimeError("Feature cache split mismatch")
    train = load_records(a.data, a.cache, "train")
    val = load_records(a.data, a.cache, "val")
    features = Features(False, architecture).cuda()
    features.load_state_dict(
        torch.load(a.cache / "pretrained-features.pt", weights_only=True)
    )
    temporal = Temporal(a.window, architecture).cuda()
    features.eval()
    pools = [[] for _ in range(12)]
    for name, r in train.items():
        for end in range(a.window - 1, len(r["labels"])):
            pools[int(r["labels"][end])].append((name, end))
    assert all(pools)
    best = -1
    history = []
    start = time.monotonic()
    previous_seconds = 0
    if resume:
        best = resume["best_score"]
        history = resume["history"]
        previous_seconds = history[-1]["seconds"]
        rng.bit_generator.state = resume["rng"]
        torch.set_rng_state(resume["torch_rng"])
        torch.cuda.set_rng_state(resume["cuda_rng"])

    for stage, epochs, batch, rate in [
        ("head", a.head_epochs, 128, 1e-3),
        ("joint", a.joint_epochs, a.joint_batch, 1e-4),
    ]:
        if resume and resume["stage"] == "joint" and stage == "head":
            continue
        for q in features.parameters():
            q.requires_grad_(False)
        if stage == "joint":
            saved = torch.load(a.output / "best.pt", weights_only=False)
            features.load_state_dict(saved["features"])
            temporal.load_state_dict(saved["temporal"])
            for block in features.trainable_tail():
                for q in block.parameters():
                    q.requires_grad_(True)
        groups = [{"params": temporal.parameters(), "lr": rate}]
        if stage == "joint":
            groups.append(
                {
                    "params": [q for q in features.parameters() if q.requires_grad],
                    "lr": 1e-5,
                }
            )
        optimizer = torch.optim.AdamW(groups, weight_decay=1e-4)
        bad = 0
        first_epoch = 0
        if resume and resume["stage"] == stage:
            features.load_state_dict(resume["features"])
            temporal.load_state_dict(resume["temporal"])
            optimizer.load_state_dict(resume["optimizer"])
            first_epoch = resume["epoch"]
            bad = resume["bad"]
            if bad >= 10 and first_epoch >= (15 if stage == "head" else 10):
                continue
        for epoch in range(first_epoch, epochs):
            temporal.train()
            features.eval()
            total = 0.0
            steps = 0
            accum = 16 // batch if stage == "joint" else 1
            optimizer.zero_grad()
            # Equal number per class; 3072 joint clips keep the small-GPU run bounded.
            samples = []
            for label, pool in enumerate(pools):
                for idx in rng.integers(
                    len(pool),
                    size=(1024 if stage == "head" else 256) * (4 if label == 0 else 1),
                ):
                    samples.append(pool[idx])
            rng.shuffle(samples)
            for i in range(0, len(samples), batch):
                chunk = samples[i : i + batch]
                ys = torch.tensor(
                    [train[n]["labels"][e] for n, e in chunk], device="cuda"
                )
                idx = [
                    np.maximum(
                        0,
                        np.rint(
                            e - np.arange(a.window - 1, -1, -1) * rng.uniform(0.8, 1.2)
                        ),
                    ).astype(int)
                    for n, e in chunk
                ]
                if stage == "head":
                    z = (
                        torch.from_numpy(
                            np.stack(
                                [
                                    train[n]["features"][ix]
                                    for (n, e), ix in zip(chunk, idx)
                                ]
                            )
                            .transpose(0, 2, 1)
                            .copy()
                        )
                        .cuda()
                        .unsqueeze(2)
                    )
                else:
                    middle = np.stack(
                        [train[n]["prefix"][ix] for (n, e), ix in zip(chunk, idx)]
                    )
                    x = torch.from_numpy(middle).float().cuda()
                    z = (
                        features.tail(x.flatten(0, 1))
                        .reshape(len(chunk), a.window, 576)
                        .transpose(1, 2)
                        .unsqueeze(2)
                    )
                z = z * (
                    1 + 0.15 * torch.randn(len(chunk), 576, 1, 1, device="cuda")
                ).clamp(0.7, 1.3)
                loss = nn.functional.cross_entropy(temporal(z), ys)
                (loss / accum).backward()
                total += float(loss.detach())
                steps += 1
                if steps % accum == 0 or i + batch >= len(samples):
                    nn.utils.clip_grad_norm_(
                        list(temporal.parameters()) + list(features.parameters()), 5
                    )
                    optimizer.step()
                    optimizer.zero_grad()
            m = evaluate(
                temporal, val, a.window, features if stage == "joint" else None
            )
            score = m["event_macro_f1"] + (
                1 if m["background_false_events_per_minute"] <= 2 else 0
            )
            row = {
                "stage": stage,
                "epoch": epoch + 1,
                "loss": total / steps,
                "seconds": previous_seconds + time.monotonic() - start,
                **m,
            }
            history.append(row)
            ck = {
                "features": features.state_dict(),
                "temporal": temporal.state_dict(),
                "window": a.window,
                "trained": True,
                "architecture": architecture,
                "split_sha256": split_hash,
                "stage": stage,
                "epoch": epoch + 1,
                "validation": m,
            }
            torch.save(ck, a.output / "last.pt")
            if score > best:
                best = score
                bad = 0
                torch.save(ck, a.output / "best.pt")
            else:
                bad += 1
            recovery = {
                **ck,
                "split_sha256": split_hash,
                "best_score": best,
                "bad": bad,
                "optimizer": optimizer.state_dict(),
                "history": history,
                "rng": rng.bit_generator.state,
                "torch_rng": torch.get_rng_state(),
                "cuda_rng": torch.cuda.get_rng_state(),
            }
            torch.save(recovery, a.output / "resume.pt.tmp")
            (a.output / "resume.pt.tmp").replace(a.output / "resume.pt")
            (a.output / "history.json").write_text(json.dumps(history, indent=2) + "\n")
            print(json.dumps(row), flush=True)
            if bad >= 10 and epoch + 1 >= (15 if stage == "head" else 10):
                break
    (a.output / "training-manifest.json").write_text(
        json.dumps(
            {
                "trained": True,
                "architecture": architecture,
                "split_sha256": split_hash,
                "seed": 42,
                "window": a.window,
                "best_validation_event_macro_f1": torch.load(
                    a.output / "best.pt", weights_only=False
                )["validation"]["event_macro_f1"],
                "seconds": previous_seconds + time.monotonic() - start,
                "device": torch.cuda.get_device_name(0),
                "test_evaluated": False,
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
