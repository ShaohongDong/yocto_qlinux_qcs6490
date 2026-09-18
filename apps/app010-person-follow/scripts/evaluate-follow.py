#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay production selection logic against manually annotated MOT17 identities."""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np
from scipy.optimize import linear_sum_assignment


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tracks', type=Path, required=True)
    p.add_argument('--ground-truth', type=Path, required=True)
    p.add_argument('--replay', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--annotation-scale', type=float, default=1)
    args = p.parse_args()
    rows = [json.loads(x) for x in args.tracks.read_text().splitlines()]
    if not rows or [r['source_frame'] for r in rows] != list(range(len(rows))) or any(r['loop'] for r in rows):
        raise ValueError('Requires complete first-pass frames')
    gt = np.loadtxt(args.ground_truth, delimiter=',', ndmin=2)
    gt = gt[(gt[:, 6] == 1) & (gt[:, 7] == 1)]
    matches, visible, first = [], [], {}
    for index, row in enumerate(rows):
        current = gt[gt[:, 0] == index+1]
        visible.append(set(current[:, 1].astype(int)))
        persons = row['persons']
        costs = np.ones((len(current), len(persons)))
        for i, item in enumerate(current):
            a = item[2:6].copy(); a[2:] += a[:2]
            for j, person in enumerate(persons):
                b = np.asarray(person['box'])*args.annotation_scale
                inter = np.maximum(0, np.minimum(a[2:], b[2:])-np.maximum(a[:2], b[:2])).prod()
                union = (a[2:]-a[:2]).prod()+(b[2:]-b[:2]).prod()-inter
                costs[i, j] = 1-inter/union if union > 0 else 1
        ii, jj = linear_sum_assignment(costs)
        mapping = {persons[j]['track_id']: int(current[i, 1]) for i, j in zip(ii, jj) if costs[i, j] <= .5}
        matches.append(mapping)
        for track, identity in mapping.items():
            first.setdefault(identity, (index, track))
    counts = dict(episodes=len(first), tracking_frames=0, correct_frames=0, wrong_identity_frames=0,
                  unmatched_tracking_frames=0, visible_frames_after_selection=0,
                  short_occlusion_recoveries=0, lost_episodes=0)
    for identity, (start, track) in sorted(first.items()):
        lines = []
        for index in range(start, len(rows)):
            r = rows[index]
            lines.append(f"{r['pts_seconds']} {r['width']} {r['height']} {track if index == start else -1} {len(r['persons'])}")
            lines.extend(' '.join(map(str, [b['track_id'], *b['box']])) for b in r['persons'])
        proc = subprocess.run([str(args.replay.resolve())], input='\n'.join(lines)+'\n', text=True, capture_output=True, check=True)
        results = [json.loads(x) for x in proc.stdout.splitlines()]
        if len(results) != len(rows)-start:
            raise ValueError('Replay frame count mismatch')
        previous = None
        for index, result in enumerate(results, start):
            counts['visible_frames_after_selection'] += identity in visible[index]
            if result['state'] == 'tracking':
                counts['tracking_frames'] += 1
                found = matches[index].get(track)
                if found == identity:
                    counts['correct_frames'] += 1
                    counts['short_occlusion_recoveries'] += previous == 'occluded'
                elif found is not None:
                    counts['wrong_identity_frames'] += 1
                else:
                    counts['unmatched_tracking_frames'] += 1
            if result['state'] == 'lost' and previous != 'lost':
                counts['lost_episodes'] += 1
            previous = result['state']
    counts['correct_follow_rate'] = counts['correct_frames']/counts['tracking_frames'] if counts['tracking_frames'] else None
    counts['visible_follow_coverage'] = counts['correct_frames']/counts['visible_frames_after_selection'] if counts['visible_frames_after_selection'] else None
    counts['protocol'] = 'One episode per GT person, select first IoU>=0.5 matched ID; no reselect; frame-weighted, overlapping episodes; production C++ Follow; not GUI or NPU acceptance'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(counts, indent=2)+'\n')


if __name__ == '__main__':
    main()
