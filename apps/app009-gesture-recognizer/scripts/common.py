# SPDX-License-Identifier: MIT
import numpy as np


def resize_rgb(image, side=224):
    h, w = image.shape[:2]
    scale = min(side / w, side / h)
    nw, nh = int(round(w * scale)), int(round(h * scale))
    left, top = (side - nw) // 2, (side - nh) // 2
    x = np.clip((np.arange(nw) + 0.5) * w / nw - 0.5, 0, w - 1)
    y = np.clip((np.arange(nh) + 0.5) * h / nh - 0.5, 0, h - 1)
    ix = x.astype(int)
    iy = y.astype(int)
    jx = np.minimum(ix + 1, w - 1)
    jy = np.minimum(iy + 1, h - 1)
    dx = (x - ix)[None, :, None]
    dy = (y - iy)[:, None, None]
    a = (
        image[iy[:, None], ix[None, :]] * (1 - dx)
        + image[iy[:, None], jx[None, :]] * dx
    )
    b = (
        image[jy[:, None], ix[None, :]] * (1 - dx)
        + image[jy[:, None], jx[None, :]] * dx
    )
    result = np.full((side, side, 3), 114, dtype=np.uint8)
    result[top : top + nh, left : left + nw] = np.floor(
        a * (1 - dy) + b * dy + 0.5
    ).astype(np.uint8)
    return result


def normalize(images):
    return (
        images.astype(np.float32) / 255 - np.array([0.485, 0.456, 0.406], np.float32)
    ) / np.array([0.229, 0.224, 0.225], np.float32)
