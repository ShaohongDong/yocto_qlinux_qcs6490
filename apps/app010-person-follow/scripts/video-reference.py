#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Offline float-model evaluation only; not an application inference fallback."""
import argparse
import json
import os
from pathlib import Path
import cv2
import numpy as np
from pose_common import preprocess, decode


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--model', type=Path, required=True)
    p.add_argument('--input', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    os.environ['TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD'] = '1'
    os.environ['YOLO_CONFIG_DIR'] = str(args.output.parent / 'yolo-config')
    import torch
    from ultralytics import YOLO
    torch.set_num_threads(4)
    if not torch.cuda.is_available():
        raise RuntimeError('CUDA required for offline float evaluation')
    model = YOLO(str(args.model)).model.cuda().eval()
    video = cv2.VideoCapture(str(args.input))
    if not video.isOpened():
        raise RuntimeError('Cannot open video')
    fps = video.get(cv2.CAP_PROP_FPS)
    expected = int(video.get(cv2.CAP_PROP_FRAME_COUNT))
    if not np.isfinite(fps) or fps <= 0:
        raise RuntimeError('Invalid video frame rate')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with args.output.open('w') as stream, torch.inference_mode():
        while True:
            ok, bgr = video.read()
            if not ok:
                break
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            raw = preprocess(rgb, 640)[None].transpose(0, 3, 1, 2).copy()
            out = model(torch.from_numpy(raw).cuda())
            output = (out[0] if isinstance(out, (tuple, list)) else out).cpu().numpy()
            persons = decode(output, rgb.shape[1], rgb.shape[0], side=640, threshold=.1)
            stream.write(json.dumps(dict(source_frame=count, loop=0, pts_seconds=count/fps,
                                         width=rgb.shape[1], height=rgb.shape[0], persons=persons,
                                         input=str(args.input), backend='float_cuda_reference'))+'\n')
            count += 1
    video.release()
    if count != expected or not count:
        raise RuntimeError(f'Incomplete decode: {count}/{expected}')
    print(f'Float reference: {count} frames')


if __name__ == '__main__':
    main()
