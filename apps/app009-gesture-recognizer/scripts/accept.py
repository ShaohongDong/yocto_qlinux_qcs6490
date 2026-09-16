#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Audit held-out accuracy and deployment identity without changing thresholds."""

import argparse
import configparser
import hashlib
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--torch", type=Path, required=True)
    p.add_argument("--onnx", type=Path, required=True)
    p.add_argument("--board", type=Path, required=True)
    p.add_argument("--model", type=Path, required=True)
    p.add_argument("--board-report", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    a = p.parse_args()
    metrics = {
        name: json.loads(getattr(a, name).read_text())
        for name in ["torch", "onnx", "board"]
    }
    selection = a.model / "selection.json"
    digest = hashlib.sha256(selection.read_bytes()).hexdigest()
    selected = json.loads(selection.read_text())
    for backend, m in metrics.items():
        if (
            m["backend"] != backend
            or m["split"] != "test"
            or m["videos"] != 40
            or m["selection_sha256"] != digest
            or any(m[k] != selected[k] for k in ["window", "threshold", "release"])
        ):
            raise ValueError("Inconsistent frozen test protocol: " + backend)
    report = json.loads(a.board_report.read_text())
    hashes = {
        n: hashlib.sha256((a.model / n).read_bytes()).hexdigest()
        for n in ["features.bin", "temporal.bin", "model.ini", "labels.txt"]
    }
    if report["status"] != "PASS" or report["resets"] or report["dropped_frames"]:
        raise ValueError("Board test did not complete exactly")
    for graph in ["features", "temporal"]:
        key = "feature" if graph == "features" else graph
        if hashes[graph + ".bin"] != report[key + "_sha256"]:
            raise ValueError("Board model identity mismatch")
        if not any(
            e["type"] == 3004 and e["value"] > 0 for e in report[graph + "_profile"]
        ):
            raise ValueError("Missing HTP execution evidence: " + graph)
    losses = {
        b: {
            k: (metrics["torch"][k] - metrics[b][k]) * 100
            for k in ["clip_top1", "clip_macro_f1", "event_macro_f1"]
        }
        for b in ["onnx", "board"]
    }
    checks = {b + "_accuracy": m["accuracy_pass"] for b, m in metrics.items()}
    checks["onnx_loss_within_0.5pp"] = max(losses["onnx"].values()) <= 0.5
    checks["quantized_cascade_loss_within_2pp"] = max(losses["board"].values()) <= 2
    config = configparser.ConfigParser()
    config.read(a.model / "model.ini")
    config.set(
        "gesture", "accuracy_accepted", "true" if all(checks.values()) else "false"
    )
    with (a.model / "model.ini").open("w") as stream:
        config.write(stream)
    hashes["model.ini"] = hashlib.sha256(
        (a.model / "model.ini").read_bytes()
    ).hexdigest()
    result = {
        "trained": True,
        "accuracy_pass": all(checks.values()),
        "checks": checks,
        "loss_percentage_points": losses,
        "selection_sha256": digest,
        "checkpoint_sha256": selected["checkpoint_sha256"],
        "sha256": hashes,
        "metrics": metrics,
        "scope": "Full frozen 40-video test split. Runtime GUI/soak acceptance is separate.",
    }
    a.output.write_text(json.dumps(result, indent=2) + "\n")
    print(
        json.dumps(
            {
                "accuracy_pass": result["accuracy_pass"],
                "checks": checks,
                "loss_percentage_points": losses,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
