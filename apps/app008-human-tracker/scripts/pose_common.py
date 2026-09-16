# SPDX-License-Identifier: MIT
"""Numerical contract shared by calibration and held-out reference evaluation."""
import numpy as np

SIDE = 416


def letterbox(width, height, side=SIDE):
    if side not in (416, 640):
        raise ValueError('Supported pose input sizes are 416 and 640')
    scale = min(side / width, side / height)
    rw, rh = max(1, round(width * scale)), max(1, round(height * scale))
    return rw, rh, (side - rw) // 2, (side - rh) // 2


def preprocess(rgb, side=SIDE):
    height, width = rgb.shape[:2]
    rw, rh, left, top = letterbox(width, height, side)
    xs = np.maximum(0, (np.arange(rw, dtype=np.float64) + .5) * width / rw - .5)
    ys = np.maximum(0, (np.arange(rh, dtype=np.float64) + .5) * height / rh - .5)
    xi, yi = xs.astype(int), ys.astype(int)
    dx, dy = (xs - xi)[None, :, None], (ys - yi)[:, None, None]
    a = rgb[yi[:, None], xi] * (1 - dx) + rgb[yi[:, None], np.minimum(xi + 1, width - 1)] * dx
    b = rgb[np.minimum(yi + 1, height - 1)[:, None], xi] * (1 - dx) + rgb[np.minimum(yi + 1, height - 1)[:, None], np.minimum(xi + 1, width - 1)] * dx
    resized = np.floor(a * (1 - dy) + b * dy + .5)
    result = np.full((side, side, 3), 114 / 255, dtype=np.float32)
    result[top:top + rh, left:left + rw] = (resized / 255).astype(np.float32)
    return result


def decode(output, width, height, threshold=.001, side=SIDE):
    anchors = sum((side // stride) ** 2 for stride in (8, 16, 32))
    output = np.asarray(output).reshape(56, anchors).T
    good = np.isfinite(output[:, :5]).all(1) & (output[:, 4] >= threshold) & (output[:, 2:4] > 0).all(1)
    rows = output[good]
    rows = rows[np.argsort(-rows[:, 4], kind='stable')[:3000]]
    if not len(rows):
        return []
    boxes = np.column_stack((rows[:, :2] - rows[:, 2:4] / 2, rows[:, :2] + rows[:, 2:4] / 2))
    area = np.prod(boxes[:, 2:4] - boxes[:, :2], axis=1)
    chosen = []
    pending = np.arange(len(rows))
    while len(pending) and len(chosen) < 300:
        i = pending[0]
        chosen.append(i)
        pending = pending[1:]
        intersection = np.prod(np.maximum(0, np.minimum(boxes[pending, 2:4], boxes[i, 2:4]) - np.maximum(boxes[pending, :2], boxes[i, :2])), axis=1)
        overlap = intersection / np.maximum(area[pending] + area[i] - intersection, 1e-12)
        pending = pending[overlap <= .7]
    rw, rh, left, top = letterbox(width, height, side)
    result = []
    for i in chosen:
        box = boxes[i].copy()
        box[[0, 2]] = np.clip((box[[0, 2]] - left) * width / rw, 0, width)
        box[[1, 3]] = np.clip((box[[1, 3]] - top) * height / rh, 0, height)
        if box[2] <= box[0] or box[3] <= box[1]:
            continue
        points = rows[i, 5:].reshape(17, 3).copy()
        points[~np.isfinite(points).all(1)] = 0
        points[:, 0] = np.clip((points[:, 0] - left) * width / rw, 0, width)
        points[:, 1] = np.clip((points[:, 1] - top) * height / rh, 0, height)
        points[:, 2] = np.clip(points[:, 2], 0, 1)
        result.append({'score': min(1., float(rows[i, 4])), 'box': box.tolist(), 'keypoints': points.tolist()})
    return result
