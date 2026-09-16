#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay cached, untracked NPU detections through the compiled C++ tracker."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--detections', type=Path, required=True)
    parser.add_argument('--replay', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--buffer', type=int, default=30)
    parser.add_argument('--high', type=float, default=.25)
    parser.add_argument('--new', type=float, default=.35)
    parser.add_argument('--match', type=float, default=.8)
    parser.add_argument('--fuse-score', action='store_true')
    args = parser.parse_args()
    rows = [json.loads(line) for line in args.detections.read_text().splitlines()]
    if not rows or any(r['loop'] != 0 for r in rows):
        raise ValueError('Replay requires one nonempty continuous video pass')
    frames = [r['source_frame'] for r in rows]
    if frames != sorted(set(frames)):
        raise ValueError('Source frames must be strictly increasing')
    tensors = []
    for row in rows:
        tensors.append(f"{row['source_frame']} {len(row['persons'])}")
        for p in row['persons']:
            values = [*p['box'], p['score'], *(v for k in p['keypoints'] for v in k)]
            if len(values) != 56:
                raise ValueError('Expected bbox/score plus 17 three-component keypoints')
            tensors.append(' '.join(map(str, values)))
    command = [str(args.replay.resolve()), str(args.buffer), str(args.high), str(args.new), str(args.match)]
    if args.fuse_score:
        command.append('1')
    result = subprocess.run(command,
                            input='\n'.join(tensors)+'\n', text=True, capture_output=True, check=True)
    out = [json.loads(line) for line in result.stdout.splitlines()]
    if [r['source_frame'] for r in out] != frames:
        raise ValueError('Tracker replay changed the frame sequence')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open('w') as stream:
        for row, tracked in zip(rows, out):
            stream.write(json.dumps({**row, 'persons': tracked['persons']})+'\n')


if __name__ == '__main__':
    main()
