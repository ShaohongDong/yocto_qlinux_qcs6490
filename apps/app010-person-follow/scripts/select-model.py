#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare float baseline/candidate on validation data, freeze selection, then test."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data', type=Path, required=True)
    p.add_argument('--weights', type=Path, required=True)
    p.add_argument('--candidate', type=Path, required=True, help='Completed train.py output directory')
    p.add_argument('--videos', type=Path, required=True, help='JSON sequence list with split, video, gt, frames, scale')
    p.add_argument('--build', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    scripts = Path(__file__).resolve().parent
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if (root/'selection.json').exists():
        raise RuntimeError('Selection already frozen; use a new output directory')
    if not (args.candidate/'training-session.json').is_file():
        raise RuntimeError('Training is incomplete')
    videos = json.loads(args.videos.read_text())
    if {v['sequence'] for v in videos if v['split']=='val'} != {'02', '10'} or {v['sequence'] for v in videos if v['split']=='test'} != {'05', '09'}:
        raise ValueError('Expected fixed MOT17 validation 02/10 and test 05/09')

    def run(name, command):
        with (root/(name+'.log')).open('w') as log:
            subprocess.run([str(x) for x in command], check=True, stdout=log, stderr=subprocess.STDOUT)

    def execute(name, *options):
        run(name, [sys.executable, '-B', *options])

    def score_model(name, weights, split):
        dest = root/name/split
        dest.mkdir(parents=True, exist_ok=True)
        execute(name+'-'+split+'-reference', scripts/'evaluate.py', 'reference', '--model', weights,
                '--data', args.data, '--split', split, '--imgsz', '640', '--device', 'cuda', '--output', dest/'coco.jsonl')
        execute(name+'-'+split+'-coco', scripts/'evaluate.py', 'pose', '--data', args.data,
                '--split', split, '--predictions', dest/'coco.jsonl', '--output', dest/'coco-metrics.json')
        mot = {}
        for v in videos:
            if v['split'] != split:
                continue
            seq = v['sequence']
            execute(name+'-'+seq+'-reference', scripts/'video-reference.py', '--model', weights,
                    '--input', v['video'], '--output', dest/f'raw{seq}.jsonl')
            execute(name+'-'+seq+'-replay', scripts/'replay-tracks.py', '--detections', dest/f'raw{seq}.jsonl',
                    '--replay', args.build/'tracker-replay', '--fuse-score', '--output', dest/f'mot{seq}.jsonl')
            execute(name+'-'+seq+'-mot', scripts/'evaluate.py', 'mot', '--ground-truth', v['gt'],
                    '--frames', v['frames'], '--annotation-scale', v['scale'], '--predictions', dest/f'mot{seq}.jsonl',
                    '--output', dest/f'mot{seq}-metrics.json')
            execute(name+'-'+seq+'-follow', scripts/'evaluate-follow.py', '--ground-truth', v['gt'],
                    '--annotation-scale', v['scale'], '--tracks', dest/f'mot{seq}.jsonl',
                    '--replay', args.build/'follow-replay', '--output', dest/f'follow{seq}-metrics.json')
            mot[seq] = json.loads((dest/f'mot{seq}-metrics.json').read_text())
        return dict(coco=json.loads((dest/'coco-metrics.json').read_text()), mot=mot)

    models = {'baseline': args.weights.resolve(), 'candidate': (args.candidate/'fit/weights/best.pt').resolve()}
    validation = {name: score_model(name, model, 'val') for name, model in models.items()}

    def aggregate(result):
        total = sum(x['num_objects'] for x in result['mot'].values())
        return dict(idf1=sum(x['idf1']*x['num_objects'] for x in result['mot'].values())/total,
                    recall=1-sum(x['num_misses'] for x in result['mot'].values())/total)

    scores = {name: aggregate(result) for name, result in validation.items()}
    a, b = scores['baseline'], scores['candidate']
    selected = 'candidate' if b['idf1']>a['idf1'] and b['recall']>=a['recall'] else 'baseline'
    dest = root/'selected';dest.mkdir()
    if selected == 'candidate':
        (dest/'fit/weights').mkdir(parents=True)
        shutil.copy2(models[selected], dest/'fit/weights/best.pt')
        shutil.copy2(args.candidate/'training-session.json', dest/'training-session.json')
    record = dict(selected=selected, trained=selected=='candidate', checkpoint_sha256=hashlib.sha256(models[selected].read_bytes()).hexdigest(),
                  criterion='GT-observation-weighted validation IDF1 strictly improves and recall does not decline; ties use baseline',
                  validation=scores, validation_sequences=['02', '10'], test_sequences=['05', '09'],
                  hardware_validation='pending; float reference only')
    (root/'selection.json').write_text(json.dumps(record, indent=2)+'\n')
    (root/'validation.json').write_text(json.dumps(validation, indent=2)+'\n')
    print(json.dumps(record), flush=True)
    execute('export-selected', scripts/'train.py', '--data', args.data, '--output', dest,
            '--weights', args.weights, '--export-only', '--imgsz', '640', '--skip-final-eval')
    final = {name: score_model(name, model, 'test') for name, model in models.items()}
    execute('onnx-test-reference', scripts/'evaluate.py', 'reference', '--model', dest/'pose.onnx', '--data', args.data,
            '--split', 'test', '--imgsz', '640', '--output', root/'onnx-test.jsonl')
    execute('onnx-test-coco', scripts/'evaluate.py', 'pose', '--data', args.data, '--split', 'test',
            '--predictions', root/'onnx-test.jsonl', '--output', root/'onnx-test-metrics.json')
    onnx = json.loads((root/'onnx-test-metrics.json').read_text())
    loss = final[selected]['coco']['bbox']['ap']-onnx['bbox']['ap']
    summary = dict(selection=record, final=final, onnx=onnx, onnx_bbox_ap_loss=loss, onnx_pass=loss<=.005)
    (root/'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    if loss>.005:
        raise RuntimeError('ONNX bbox AP loss exceeds 0.5 pp')


if __name__ == '__main__':
    main()
