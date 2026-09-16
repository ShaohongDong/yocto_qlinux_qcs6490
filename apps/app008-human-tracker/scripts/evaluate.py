#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reference inference and COCO/MOT evaluation with explicit held-out inputs."""
import argparse
import json
import os
from pathlib import Path
import sys
import numpy as np
from PIL import Image
from pose_common import preprocess, decode


def save(value, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + '\n')


def reference(args):
    root = args.data.resolve()
    meta = json.loads((root / f'{args.split}-coco.json').read_text())
    png = args.output.parent / f'{args.split}-images'
    png.mkdir(parents=True, exist_ok=True)
    entries = []
    if args.model.suffix == '.onnx':
        import onnxruntime as ort
        options = ort.SessionOptions()
        options.intra_op_num_threads = 4
        model = ort.InferenceSession(str(args.model), sess_options=options, providers=['CPUExecutionProvider'])
        infer = lambda raw: model.run(None, {model.get_inputs()[0].name: raw})[0]
    else:
        os.environ['TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD'] = '1'
        os.environ['YOLO_CONFIG_DIR'] = str(args.output.parent / 'yolo-config')
        import torch
        from ultralytics import YOLO
        torch.set_num_threads(4)
        device = torch.device(args.device)
        model = YOLO(str(args.model)).model.to(device).eval()
        def infer(raw):
            with torch.inference_mode():
                out = model(torch.from_numpy(raw).to(device))
                return (out[0] if isinstance(out, (tuple, list)) else out).cpu().numpy()
    with args.output.open('w') as stream:
        for number, im in enumerate(sorted(meta['images'], key=lambda im: im['id'])):
            name = f"{im['id']:012d}.png"
            target = png / name
            if not target.exists():
                with Image.open(root / 'images' / args.split / im['file_name']) as source:
                    source.convert('RGB').save(target)
            rgb = np.array(Image.open(target).convert('RGB'))
            raw = preprocess(rgb, args.imgsz)[None].transpose(0, 3, 1, 2).copy()
            persons = decode(infer(raw), im['width'], im['height'], side=args.imgsz)
            stream.write(json.dumps({'frame': number, 'input': name, 'width': im['width'], 'height': im['height'], 'persons': persons}) + '\n')
            entries.append(f'{args.split}-images/' + name)
            if (number + 1) % 100 == 0:
                print(f'Reference {number + 1}/{len(meta["images"])}', flush=True)
    (args.output.parent / f'{args.split}-list.txt').write_text('\n'.join(entries) + '\n')


def pose_metrics(args):
    from pycocotools.coco import COCO
    from pycocotools.cocoeval import COCOeval
    gt = COCO(str(args.data / f'{args.split}-coco.json'))
    predictions, ids = [], []
    for line in args.predictions.read_text().splitlines():
        row = json.loads(line)
        image_id = int(Path(row['input']).stem)
        ids.append(image_id)
        for person in row['persons']:
            x1, y1, x2, y2 = person['box']
            predictions.append({'image_id': image_id, 'category_id': 1, 'score': person['score'],
                                'bbox': [x1, y1, x2-x1, y2-y1],
                                'keypoints': np.asarray(person['keypoints']).flatten().tolist()})
    if len(ids) != len(set(ids)) or set(ids) != set(gt.getImgIds()):
        raise ValueError('Predictions must contain each selected split image exactly once')
    if not predictions:
        raise ValueError('No predictions; inference/accuracy acceptance failed')
    detections = gt.loadRes(predictions)
    metrics = {'images': len(ids), 'persons': len(predictions), 'split': args.split,
               'protocol': f'COCOeval on fixed {args.split} subset; confidence .001, NMS IoU .7'}
    for kind in ('keypoints', 'bbox'):
        evaluator = COCOeval(gt, detections, kind)
        evaluator.params.imgIds = sorted(ids)
        evaluator.evaluate(); evaluator.accumulate(); evaluator.summarize()
        metrics[kind] = {'ap': float(evaluator.stats[0]), 'ap50': float(evaluator.stats[1]), 'ap75': float(evaluator.stats[2])}
    save(metrics, args.output)


def mot_metrics(args):
    import motmetrics as mm
    from scipy.optimize import linear_sum_assignment
    gt = np.loadtxt(args.ground_truth, delimiter=',', ndmin=2)
    rows = [json.loads(line) for line in args.predictions.read_text().splitlines()]
    # Only the first continuous pass is used, never aggregate restarted IDs.
    rows = [r for r in rows if r['loop'] == 0]
    if [r['source_frame'] for r in rows] != list(range(args.frames)):
        raise ValueError('Tracking evaluation requires all frames in order, without GUI dropping')
    acc = mm.MOTAccumulator(auto_id=True)
    ignored = 0
    for row in rows:
        current = gt[gt[:, 0] == row['source_frame'] + 1]
        # MOT17 distractor classes: person on vehicle, static person, distractor,
        # reflection. Remove predictions assigned to these before scoring.
        pred = row['persons']
        boxes = np.asarray([p['box'] for p in pred], dtype=float).reshape(-1, 4)
        if len(boxes):
            boxes[:, 2:] -= boxes[:, :2]
        boxes *= args.annotation_scale
        remove = set()
        if len(current) and len(boxes):
            d = mm.distances.iou_matrix(current[:, 2:6], boxes, max_iou=.5)
            r, c = linear_sum_assignment(np.nan_to_num(d, nan=1e6))
            remove = {j for i, j in zip(r, c) if np.isfinite(d[i, j]) and int(current[i, 7]) in (2, 7, 8, 12)}
        keep = [i for i in range(len(pred)) if i not in remove]
        ignored += len(remove)
        current = current[(current[:, 6] == 1) & (current[:, 7] == 1)]
        acc.update(current[:, 1].astype(int).tolist(), [pred[i]['track_id'] for i in keep],
                   mm.distances.iou_matrix(current[:, 2:6], boxes[keep], max_iou=.5))
    names = ['idf1', 'idp', 'idr', 'mota', 'motp', 'num_switches', 'num_fragmentations', 'num_misses',
             'num_false_positives', 'num_objects', 'num_frames', 'mostly_tracked', 'mostly_lost']
    result = mm.metrics.create().compute(acc, metrics=names, name='sequence').iloc[0].to_dict()
    result = {key: float(value) if np.isfinite(value) else None for key, value in result.items()}
    result.update(ignored_distractor_predictions=ignored, annotation_scale=args.annotation_scale,
                  protocol='MOT17 manual GT class 1, valid=1; IoU>=0.5; distractor-assigned predictions excluded; archived raw video; not an official benchmark submission')
    save(result, args.output)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='command', required=True)
    r = sub.add_parser('reference'); r.add_argument('--model', type=Path, required=True)
    r.add_argument('--data', type=Path, required=True); r.add_argument('--output', type=Path, required=True)
    r.add_argument('--split', choices=('val', 'test'), default='test')
    r.add_argument('--device', choices=('cpu', 'cuda'), default='cpu')
    r.add_argument('--imgsz', type=int, choices=(416, 640), default=416)
    k = sub.add_parser('pose'); k.add_argument('--data', type=Path, required=True)
    k.add_argument('--predictions', type=Path, required=True); k.add_argument('--output', type=Path, required=True)
    k.add_argument('--split', choices=('val', 'test'), default='test')
    m = sub.add_parser('mot'); m.add_argument('--ground-truth', type=Path, required=True)
    m.add_argument('--predictions', type=Path, required=True); m.add_argument('--output', type=Path, required=True)
    m.add_argument('--frames', type=int, required=True); m.add_argument('--annotation-scale', type=float, default=1)
    args = p.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    {'reference': reference, 'pose': pose_metrics, 'mot': mot_metrics}[args.command](args)


if __name__ == '__main__':
    main()
