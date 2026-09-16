# SPDX-License-Identifier: MIT
"""Causal single-hand frontend. Model geometry follows MediaPipe Hands.

Upstream graph configuration: google-ai-edge/mediapipe (Apache-2.0),
mediapipe/modules/{palm_detection,hand_landmark}. No MediaPipe runtime on Q6A.
All coordinates here use source image pixels; model tensors use RGB [0, 1].
"""

import math
from pathlib import Path
import numpy as np


def anchors():
    return np.asarray(
        [
            ((x + 0.5) / n, (y + 0.5) / n)
            for n, copies in [(24, 2), (12, 6)]
            for y in range(n)
            for x in range(n)
            for _ in range(copies)
        ],
        np.float32,
    )


ANCHORS = anchors()
GRID = {}


def sample_rect(rgb, rect, side):
    """Pixel-centre bilinear square affine crop, zero padding, round uint8."""
    if side not in GRID:
        t = (np.arange(side, dtype=np.float64) + 0.5) / side - 0.5
        GRID[side] = np.meshgrid(t, t)
    u, v = GRID[side]
    cx, cy, size, angle = rect
    co, si = math.cos(angle), math.sin(angle)
    x = cx + size * (co * u - si * v) - 0.5
    y = cy + size * (si * u + co * v) - 0.5
    ix, iy = np.floor(x).astype(np.int32), np.floor(y).astype(np.int32)
    dx, dy = (x - ix)[..., None], (y - iy)[..., None]
    h, w = rgb.shape[:2]

    def get(xx, yy):
        mask = ((xx >= 0) & (xx < w) & (yy >= 0) & (yy < h))[..., None]
        return rgb[np.clip(yy, 0, h - 1), np.clip(xx, 0, w - 1)] * mask

    result = (get(ix, iy) * (1 - dx) + get(ix + 1, iy) * dx) * (1 - dy) + (
        get(ix, iy + 1) * (1 - dx) + get(ix + 1, iy + 1) * dx
    ) * dy
    return np.floor(result + 0.5).astype(np.uint8)


def palm_input(rgb):
    h, w = rgb.shape[:2]
    return sample_rect(rgb, (w / 2, h / 2, max(w, h), 0), 192).astype(np.float32) / 255


def decode_palms(boxes, logits, width, height, threshold=0.5):
    boxes = np.asarray(boxes).reshape(2016, 18)
    score = 1 / (1 + np.exp(-np.clip(np.asarray(logits).reshape(2016), -80, 80)))
    ids = np.flatnonzero(score >= threshold)
    if not len(ids):
        return []
    values = boxes[ids].copy() / 192
    values[:, :2] += ANCHORS[ids]
    values[:, 4:] = (values[:, 4:].reshape(-1, 7, 2) + ANCHORS[ids, None]).reshape(
        -1, 14
    )
    xy = values[:, :2]
    wh = values[:, 2:4]
    bounds = np.concatenate([xy - wh / 2, xy + wh / 2], 1)
    remaining = np.argsort(-score[ids])
    result = []
    while len(remaining):
        i = remaining[0]
        other = bounds[remaining]
        area = np.maximum(0, other[:, 2:] - other[:, :2]).prod(1)
        overlap = np.maximum(
            0,
            np.minimum(other[:, 2:], bounds[i, 2:])
            - np.maximum(other[:, :2], bounds[i, :2]),
        ).prod(1)
        iou = overlap / np.maximum(
            area + np.maximum(0, bounds[i, 2:] - bounds[i, :2]).prod() - overlap, 1e-9
        )
        group = remaining[iou > 0.3]
        remaining = remaining[iou <= 0.3]
        if not len(group):  # Reject degenerate boxes without looping forever.
            remaining = remaining[remaining != i]
            continue
        b = np.average(values[group], axis=0, weights=score[ids[group]])
        scale = max(width, height)
        origin = np.array([(width - scale) / 2, (height - scale) / 2])
        center = b[:2] * scale + origin
        size = b[2:4] * scale
        kp = b[4:].reshape(7, 2) * scale + origin
        delta = kp[2] - kp[0]
        angle = math.pi / 2 + math.atan2(delta[1], delta[0])
        angle = (angle + math.pi) % (2 * math.pi) - math.pi
        center += 0.5 * size[1] * np.array([math.sin(angle), -math.cos(angle)])
        length = float(max(size) * 2.6)
        if length >= 8 and np.isfinite(b).all():
            result.append(
                {
                    "rect": np.array([*center, length, angle]),
                    "score": float(score[ids[i]]),
                }
            )
    return result


def project_landmarks(raw, rect):
    x = np.asarray(raw).reshape(21, 3).astype(np.float64) / 224
    cx, cy, size, angle = rect
    co, si = math.cos(angle), math.sin(angle)
    uv = x[:, :2] - 0.5
    x[:, 0] = cx + size * (co * uv[:, 0] - si * uv[:, 1])
    x[:, 1] = cy + size * (si * uv[:, 0] + co * uv[:, 1])
    x[:, 2] *= size / 0.4
    return x


def landmark_rect(points):
    delta = points[[5, 9, 13], :2].mean(0) - points[0, :2]
    angle = math.pi / 2 + math.atan2(delta[1], delta[0])
    angle = (angle + math.pi) % (2 * math.pi) - math.pi
    co, si = math.cos(angle), math.sin(angle)
    rot = np.array([[co, si], [-si, co]])
    local = points[:, :2] @ rot.T
    lo, hi = local.min(0), local.max(0)
    center = (lo + hi) / 2
    center[1] -= 0.1 * (hi[1] - lo[1])
    center = center @ rot
    return np.array([*center, max(hi - lo) * 2, angle])


class LiteModel:
    """PC reference only; deployment supplies actual HTP inference callbacks."""

    def __init__(self, path):
        from tflite_runtime.interpreter import Interpreter

        self.model = Interpreter(str(path), num_threads=1)
        self.model.allocate_tensors()
        self.input = self.model.get_input_details()[0]["index"]
        self.outputs = self.model.get_output_details()

    def __call__(self, image):
        self.model.set_tensor(self.input, image[None].astype(np.float32))
        self.model.invoke()
        return {x["name"]: self.model.get_tensor(x["index"]) for x in self.outputs}


class HandFrontend:
    def __init__(self, models=None, palm=None, landmark=None):
        self.palm = palm or LiteModel(Path(models) / "palm_detection_lite.tflite")
        self.landmark = landmark or LiteModel(
            Path(models) / "hand_landmark_lite.tflite"
        )
        self.reset()

    def reset(self):
        self.rect = None
        self.previous = None
        self.last_detection = -1e9
        self.last_valid = -1e9
        self.previous_time = None

    def step(self, rgb, now):
        h, w = rgb.shape[:2]
        switched = False
        if self.rect is None or now - self.last_detection >= 0.2 - 1e-6:
            output = self.palm(palm_input(rgb))
            palms = decode_palms(output["Identity"], output["Identity_1"], w, h)
            self.last_detection = now
            if palms:
                if self.rect is None:
                    chosen = palms[0]
                else:
                    chosen = min(
                        palms,
                        key=lambda p: np.linalg.norm(p["rect"][:2] - self.rect[:2])
                        / max(self.rect[2], 1)
                        - 0.1 * p["score"],
                    )
                switched = (
                    self.rect is not None
                    and np.linalg.norm(chosen["rect"][:2] - self.rect[:2])
                    > self.rect[2]
                )
                self.rect = chosen["rect"]
        geometry = np.zeros(134, np.float32)
        crop = np.zeros((224, 224, 3), np.uint8)
        valid = False
        confidence = 0.0
        points = None
        used_rect = None
        if self.rect is not None:
            used_rect = self.rect.copy()
            crop = sample_rect(rgb, used_rect, 224)
            out = self.landmark(crop.astype(np.float32) / 255)
            confidence = float(out["Identity_1"].reshape(-1)[0])
            points = project_landmarks(out["Identity"], used_rect)
            valid = confidence >= 0.5 and np.isfinite(points).all()
            if valid:
                scale = max(float(np.linalg.norm(points[9] - points[0])), 1.0)
                relative = ((points - points[0]) / scale).reshape(-1)
                wrist = points[0, :2] / [w, h]
                box = np.ptp(points[:, :2], axis=0) / [w, h]
                dt = now - self.previous_time if self.previous_time is not None else 0
                velocity = np.zeros(63)
                wrist_velocity = np.zeros(2)
                if self.previous is not None and 0 < dt <= 0.15 and not switched:
                    velocity = np.clip((relative - self.previous[:63]) / dt, -20, 20)
                    wrist_velocity = np.clip(
                        (wrist - self.previous[63:65]) / dt, -10, 10
                    )
                geometry[:] = np.concatenate(
                    [relative, velocity, wrist, box, wrist_velocity, [confidence, 1]]
                )
                self.previous = np.concatenate([relative, wrist])
                self.previous_time = now
                self.last_valid = now
                self.rect = landmark_rect(points)
            else:
                self.rect = None
                self.previous = None
                self.previous_time = None
        reset = switched or (now - self.last_valid >= 0.5)
        if reset:
            self.previous = None
            self.previous_time = None
        if not valid:
            crop.fill(0)
        return {
            "rgb": crop,
            "geometry": geometry,
            "valid": valid,
            "reset": reset,
            "confidence": confidence,
            "points": points,
            "rect": self.rect,
            "input_rect": used_rect,
        }
