#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded v2 experiments, training/validation only. No historical test access."""

import argparse, copy, hashlib, json, time
from pathlib import Path
import cv2, numpy as np, torch
from torch import nn
from hand_model import HandTemporal, Appearance, CHANNELS
from events import Decoder, event_metrics
from common import normalize


def load_records(data, cache, branch, window, subset):
    videos = json.loads((data / "split.json").read_text())["videos"]
    records = {}
    for name, v in videos.items():
        if v["split"] != subset:
            continue
        d = cache / name
        if not (
            d / ("complete.json" if branch == "geometry" else "features-complete.json")
        ).exists():
            raise RuntimeError("Incomplete features " + name)
        arrays = []
        if branch in ["absolute", "roi", "fusion"]:
            arrays.append(
                np.load(
                    d / ("absolute.npy" if branch == "absolute" else "roi.npy")
                ).astype(np.float32)
            )
        if branch in ["geometry", "fusion"]:
            arrays.append(np.load(d / "geometry.npy"))
        x = np.concatenate(arrays, 1)
        frames = np.load(d / "frames.npy")
        reset = (
            np.load(d / "reset.npy") if branch != "absolute" else np.zeros(len(x), bool)
        )
        reset[0] = True
        starts = np.maximum.accumulate(np.where(reset, np.arange(len(x)), 0))
        ready = np.arange(len(x)) - starts >= window - 1
        records[name] = {
            "x": x,
            "frames": frames,
            "times": frames / 30,
            "reset": reset,
            "ready": ready,
            "labels": np.load(d / "labels.npy"),
            "meta": v,
            "dir": d,
        }
    return records


def probabilities(model, records, window):
    model.eval()
    allp = {}
    with torch.inference_mode():
        for name, r in records.items():
            ps = np.zeros((len(r["x"]), 12), np.float32)
            ps[:, 0] = 1
            ends = np.flatnonzero(r["ready"])
            for i in range(0, len(ends), 128):
                ix = ends[i : i + 128, None] - np.arange(window - 1, -1, -1)
                x = (
                    torch.from_numpy(r["x"][ix].transpose(0, 2, 1).copy())
                    .cuda()
                    .unsqueeze(2)
                )
                ps[ends[i : i + 128]] = torch.softmax(model(x), 1).cpu().numpy()
            allp[name] = ps
    return allp


def measure(records, ps, threshold, release, stable):
    truth = {}
    pred = {}
    bg = 0
    conf = np.zeros((12, 12), np.int64)
    for name, r in records.items():
        gt = [
            {
                "label": s["label"],
                "start": s["start_frame"] / 30,
                "end": s["end_frame"] / 30,
            }
            for s in r["meta"]["segments"]
            if s["label"]
        ]
        truth[name] = gt
        bg += sum(
            (s["end_frame"] - s["start_frame"]) / 30
            for s in r["meta"]["segments"]
            if not s["label"]
        )
        decoder = Decoder(
            threshold,
            release,
            sample_period=0.05,
            min_duration=0.1,
            stable_seconds=stable,
        )
        for i, (p, t) in enumerate(zip(ps[name], r["times"])):
            if r["reset"][i]:
                decoder.close(float(t), False)
            decoder.update(p, float(t))
        decoder.close(r["meta"]["frames"] / 30, False)
        pred[name] = decoder.events
        for s in gt:
            mask = (r["times"] >= s["start"]) & (r["times"] < s["end"])
            v = ps[name][mask]
            label = int(v.mean(0).argmax()) if len(v) else 0
            conf[s["label"], label] += 1
    m = event_metrics(pred, truth, bg)
    tp = np.diag(conf)
    m.update(
        clip_top1=float(tp.sum() / max(conf.sum(), 1)),
        clip_macro_f1=float(
            (2 * tp / np.maximum(conf.sum(0) + conf.sum(1), 1))[1:].mean()
        ),
        clip_confusion=conf.tolist(),
        threshold=threshold,
        release=release,
        stable_seconds=stable,
        sample_hz=20,
    )
    return m


def score(m):
    return (
        m["background_false_events_per_minute"] <= 2,
        m["event_macro_f1"],
        m["clip_macro_f1"],
        -(m["confirmation_delay_p95_seconds"] or 0),
    )


def evaluate(model, records, window, settings=None):
    ps = probabilities(model, records, window)
    if settings:
        return measure(records, ps, *settings), ps
    best = None
    for threshold in [0.35, 0.5, 0.65, 0.8, 0.9]:
        for release in [0.2, 0.35, 0.5]:
            for stable in [0.1, 0.15, 0.2]:
                m = measure(records, ps, threshold, release, stable)
                if best is None or score(m) > score(best):
                    best = m
    return best, ps


def make_pools(records):
    # Class -> person -> action interval -> eligible causal endpoints.
    pools = [{} for _ in range(12)]
    for name, r in records.items():
        person = "_".join(name.split("_")[:2])
        frames = r["frames"]
        for seg in r["meta"]["segments"]:
            ids = np.flatnonzero(
                r["ready"]
                & (frames >= seg["start_frame"])
                & (frames < seg["end_frame"])
            )
            if len(ids):
                pools[seg["label"]].setdefault(person, []).append((name, ids))
    if not all(pools):
        raise ValueError("Missing eligible training class")
    return pools


def samples(pools, rng, count):
    result = []
    for i in range(count):
        label = 0 if i % 2 == 0 else int(rng.integers(1, 12))
        persons = list(pools[label])
        person = persons[int(rng.integers(len(persons)))]
        segments = pools[label][person]
        name, ids = segments[int(rng.integers(len(segments)))]
        result.append((name, int(ids[int(rng.integers(len(ids)))])))
    rng.shuffle(result)
    return result


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--cache", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--branch", choices=list(CHANNELS), required=True)
    p.add_argument("--window", type=int, choices=[32, 64], default=32)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--epochs", type=int, default=30)
    p.add_argument("--deadline", type=float, required=True)
    a = p.parse_args()
    a.output.mkdir(exist_ok=False)
    torch.set_num_threads(2)
    torch.manual_seed(a.seed)
    np.random.seed(a.seed)
    rng = np.random.default_rng(a.seed)
    train = load_records(a.data, a.cache, a.branch, a.window, "train")
    val = load_records(a.data, a.cache, a.branch, a.window, "val")
    pools = make_pools(train)
    values = np.concatenate([r["x"][::10] for r in train.values()])
    model = HandTemporal(a.branch, a.window, values.mean(0), values.std(0)).cuda()
    optimizer = torch.optim.AdamW(model.parameters(), lr=1e-3, weight_decay=1e-4)
    start = time.monotonic()
    history = []
    best = None
    bad = 0
    hard = []
    for epoch in range(a.epochs):
        if time.time() >= a.deadline:
            break
        model.train()
        total = 0
        steps = 0
        chosen = samples(pools, rng, 8192)
        # Hard backgrounds are mined from training predictions only.
        if hard:
            for j in range(0, len(chosen), 4):
                chosen[j] = hard[int(rng.integers(len(hard)))]
        for i in range(0, len(chosen), 64):
            chunk = chosen[i : i + 64]
            xs = []
            ys = []
            for name, end in chunk:
                # Stay inside valid causal history; never reverse or use future frames.
                speed = rng.uniform(0.8, 1.2)
                ix = np.maximum(
                    0,
                    np.rint(end - np.arange(a.window - 1, -1, -1) * speed).astype(int),
                )
                if train[name]["reset"][ix[0] + 1 : end + 1].any():
                    ix = end - np.arange(a.window - 1, -1, -1)
                xs.append(train[name]["x"][ix])
                ys.append(train[name]["labels"][ix])
            x = (
                torch.from_numpy(np.stack(xs).transpose(0, 2, 1).copy())
                .cuda()
                .unsqueeze(2)
            )
            y = torch.from_numpy(np.stack(ys)).cuda()
            x = x * (1 + 0.05 * torch.randn(len(chunk), 1, 1, 1, device="cuda")).clamp(
                0.85, 1.15
            )
            optimizer.zero_grad()
            logits = model.sequence(x)[:, :, 0, :]
            # Earlier timesteps have shorter context; warm first half is unsupervised.
            loss = nn.functional.cross_entropy(
                logits[:, :, -a.window // 2 :], y[:, -a.window // 2 :]
            )
            loss.backward()
            nn.utils.clip_grad_norm_(model.parameters(), 5)
            optimizer.step()
            total += float(loss.detach())
            steps += 1
        m, ps = evaluate(model, val, a.window)
        row = {
            "stage": "head",
            "epoch": epoch + 1,
            "loss": total / max(steps, 1),
            "seconds": time.monotonic() - start,
            **m,
        }
        history.append(row)
        if best is None or score(m) > score(best):
            best = m
            bad = 0
            ck = {
                "format_version": 2,
                "branch": a.branch,
                "window": a.window,
                "sample_hz": 20,
                "seed": a.seed,
                "temporal": model.cpu().state_dict(),
                "validation": m,
                "epoch": epoch + 1,
                "split_sha256": hashlib.sha256(
                    (a.data / "split.json").read_bytes()
                ).hexdigest(),
                "cache_identity": json.loads((a.cache / "identity.json").read_text()),
            }
            torch.save(ck, a.output / "best.pt")
            model.cuda()
            for name, v in ps.items():
                np.save(a.output / (name + ".npy"), v)
        else:
            bad += 1
        (a.output / "history.json").write_text(json.dumps(history, indent=2) + "\n")
        print(
            json.dumps(
                {
                    k: v
                    for k, v in row.items()
                    if k
                    in [
                        "epoch",
                        "loss",
                        "seconds",
                        "clip_top1",
                        "clip_macro_f1",
                        "event_macro_f1",
                        "background_false_events_per_minute",
                    ]
                }
            ),
            flush=True,
        )
        if (epoch + 1) % 5 == 0:
            # Bounded subset, no validation mining; refresh false-action backgrounds.
            subset = {
                n: r
                for j, (n, r) in enumerate(train.items())
                if j % 10 == epoch // 5 % 10
            }
            pp = probabilities(model, subset, a.window)
            hard = [
                (n, int(i))
                for n, r in subset.items()
                for i in np.flatnonzero(
                    r["ready"] & (r["labels"] == 0) & (pp[n][:, 1:].max(1) > 0.35)
                )
            ]
            hard = hard[:10000]
        if bad >= 5 and epoch + 1 >= 10:
            break
    if best is None:
        raise RuntimeError("Budget expired before first completed epoch")
    (a.output / "training-manifest.json").write_text(
        json.dumps(
            {
                "trained": True,
                "format_version": 2,
                "branch": a.branch,
                "window": a.window,
                "seed": a.seed,
                "epochs": len(history),
                "seconds": time.monotonic() - start,
                "validation": best,
                "test_evaluated": False,
                "stage": "head",
                "budget_exhausted": time.time() >= a.deadline,
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
