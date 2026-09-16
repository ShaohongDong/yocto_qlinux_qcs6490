#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Freeze a candidate using validation only, before any held-out test evaluation."""

import argparse, datetime, hashlib, json, shutil
from pathlib import Path
import torch
from model import architecture_name


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", type=Path, required=True)
    p.add_argument("--runs", nargs="+", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    if a.output.exists():
        raise RuntimeError("Selection is immutable; use a new experiment directory")
    candidates = []
    split_hash = hashlib.sha256((a.data / "split.json").read_bytes()).hexdigest()
    for run in a.runs:
        manifest = json.loads((run / "training-manifest.json").read_text())
        if not manifest.get("trained") or manifest.get("test_evaluated"):
            raise RuntimeError("Candidate is not training-only")
        ck = torch.load(run / "best.pt", map_location="cpu", weights_only=False)
        if any(x.get("split_sha256") != split_hash for x in [manifest, ck]):
            raise RuntimeError("Candidate training split mismatch")
        m = ck["validation"]
        key = (
            m["background_false_events_per_minute"] <= 2,
            m["event_macro_f1"],
            m["clip_macro_f1"],
            -ck["window"],
        )
        candidates.append((key, run, ck, m))
    _, run, ck, m = max(candidates, key=lambda x: x[0])
    a.output.mkdir(parents=True)
    shutil.copy2(run / "best.pt", a.output / "best.pt")
    shutil.copy2(run / "training-manifest.json", a.output / "training-manifest.json")
    result = {
        "selected_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "selection_source": "validation only",
        "selected_run": str(run.resolve()),
        "window": ck["window"],
        "architecture": architecture_name(ck.get("architecture", "global")),
        "threshold": m["threshold"],
        "release": m["release"],
        "validation": m,
        "split_sha256": hashlib.sha256(
            (a.data / "split.json").read_bytes()
        ).hexdigest(),
        "checkpoint_sha256": hashlib.sha256(
            (a.output / "best.pt").read_bytes()
        ).hexdigest(),
        "candidates": {str(r): v for k, r, c, v in candidates},
    }
    (a.output / "selection.json").write_text(json.dumps(result, indent=2) + "\n")
    (a.output / "model.ini").write_text(
        f'[gesture]\nwindow={ck["window"]}\nthreshold={m["threshold"]}\nrelease={m["release"]}\n'
    )
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
