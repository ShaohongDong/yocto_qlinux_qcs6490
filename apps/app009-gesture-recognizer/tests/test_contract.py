#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse, sys, tempfile, subprocess
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from common import resize_rgb, normalize
from events import Decoder

p = argparse.ArgumentParser()
p.add_argument("--probe", type=Path, required=True)
a = p.parse_args()
rng = np.random.default_rng(42)
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    for w, h in [(1, 1), (7, 5), (959, 539), (333, 777), (1280, 720)]:
        rgb = rng.integers(0, 256, (h, w, 3), np.uint8)
        rgb.tofile(root / "in")
        subprocess.run(
            [
                str(a.probe.resolve()),
                "preprocess",
                str(w),
                str(h),
                str(root / "in"),
                str(root / "out"),
            ],
            check=True,
        )
        actual = np.fromfile(root / "out", np.float32).reshape(224, 224, 3)
        np.testing.assert_allclose(
            actual, normalize(resize_rgb(rgb)), atol=1e-6, rtol=0
        )
    rows = []
    d = Decoder()
    for i in range(300):
        label = (
            3
            if i < 40
            else (
                0 if i < 55 else 4 if i < 150 else 5 if i < 200 else 0 if i < 280 else 1
            )
        )
        scores = np.full(12, 0.001)
        scores[label] = 0.989
        d.update(scores, i / 10)
        rows.append(" ".join(map(str, [i / 10, *scores])))
    d.close(999, False)
    (root / "in").write_text("\n".join(rows) + "\n")
    subprocess.run(
        [
            str(a.probe.resolve()),
            "events",
            "0",
            "0",
            str(root / "in"),
            str(root / "out"),
        ],
        check=True,
    )
    actual = np.loadtxt(root / "out", ndmin=2)
    expected = np.array(
        [
            [
                x["label"],
                x["start"],
                x["end"],
                x["confirmed_at"],
                x["confidence"],
                x["complete"],
            ]
            for x in d.events
        ]
    )
    np.testing.assert_allclose(actual, expected, atol=1e-6, rtol=0)
print("Python/C++ image preprocessing and continuous event contracts PASS")
