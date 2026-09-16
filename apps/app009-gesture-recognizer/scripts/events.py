# SPDX-License-Identifier: MIT
"""Causal event confirmation, also mirrored by the C++ application."""

import numpy as np


class Decoder:
    def __init__(
        self,
        threshold=0.6,
        release=0.6,
        stable=3,
        sample_period=0.1,
        min_duration=0.2,
        stable_seconds=None,
    ):
        if not (
            0 < threshold < 1
            and 0.01 <= release <= 3
            and 0 < sample_period <= 1
            and min_duration >= 0
            and (stable_seconds is None or 0 < stable_seconds <= 1)
        ):
            raise ValueError("Invalid event settings")
        self.sample_period = sample_period
        self.min_duration = min_duration
        self.stable_seconds = stable_seconds
        self.threshold = threshold
        self.release = release
        self.stable = stable
        self.active = 0
        self.pending = 0
        self.count = 0
        self.start = 0.0
        self.pending_start = 0.0
        self.last = 0.0
        self.score = 0.0
        self.events = []

    def close(self, now, complete=True):
        if self.active and self.last - self.start >= self.min_duration - 1e-6:
            self.events.append(
                {
                    "label": self.active,
                    "start": self.start,
                    "end": self.last + self.sample_period,
                    "confirmed_at": now,
                    "confidence": self.score,
                    "complete": complete,
                }
            )
        self.active = 0
        self.pending = 0
        self.count = 0

    def update(self, p, now):
        label = int(np.argmax(p))
        label = label if label and p[label] >= self.threshold else 0
        if label and label == self.active:
            self.last = now
            self.score = max(self.score, float(p[label]))
            self.pending = 0
            self.count = 0
        elif label:
            if label == self.pending:
                self.count += 1
            else:
                self.pending = label
                self.pending_start = now
                self.count = 1
            confirmed = (
                self.count >= self.stable
                if self.stable_seconds is None
                else now - self.pending_start + self.sample_period
                >= self.stable_seconds - 1e-6
            )
            if confirmed:
                if self.active:
                    self.close(now)
                self.active = label
                self.start = self.pending_start
                self.last = now
                self.score = float(p[label])
                self.pending = 0
                self.count = 0
        else:
            self.pending = 0
            self.count = 0
            if self.active and now - self.last >= self.release - 1e-6:
                self.close(now)


def event_metrics(predictions, truth, background_seconds):
    tp = np.zeros(12)
    fp = np.zeros(12)
    fn = np.zeros(12)
    false_background = 0
    latency = []
    for name, gt in truth.items():
        events = [e for e in predictions.get(name, []) if e.get("complete", True)]
        used = set()
        for e in sorted(events, key=lambda x: -x["confidence"]):
            choices = []
            for i, t in enumerate(gt):
                if i in used or t["label"] != e["label"]:
                    continue
                overlap = max(0, min(e["end"], t["end"]) - max(e["start"], t["start"]))
                union = max(e["end"], t["end"]) - min(e["start"], t["start"])
                if union and overlap / union >= 0.3:
                    choices.append((overlap / union, i))
            if choices:
                _, i = max(choices)
                used.add(i)
                tp[e["label"]] += 1
                latency.append(e["confirmed_at"] - gt[i]["end"])
            else:
                fp[e["label"]] += 1
                if not any(
                    min(e["end"], t["end"]) > max(e["start"], t["start"]) for t in gt
                ):
                    false_background += 1
        for i, t in enumerate(gt):
            if i not in used:
                fn[t["label"]] += 1
    f1 = 2 * tp / np.maximum(2 * tp + fp + fn, 1)
    return {
        "event_macro_f1": float(f1[1:].mean()),
        "per_class_f1": f1[1:].tolist(),
        "tp": tp.tolist(),
        "fp": fp.tolist(),
        "fn": fn.tolist(),
        "background_false_events_per_minute": false_background
        / max(background_seconds / 60, 1e-9),
        "confirmation_delay_p50_seconds": (
            float(np.median(latency)) if latency else None
        ),
        "confirmation_delay_p95_seconds": (
            float(np.percentile(latency, 95)) if latency else None
        ),
        "matching": "same class, one-to-one, temporal IoU >= 0.3; duplicate events are false positives",
    }
