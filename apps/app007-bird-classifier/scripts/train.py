#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproducible CUB classes 1..20 training, export and calibration assets."""
import argparse
import hashlib
import json
import random
import subprocess
import time
from pathlib import Path

import numpy as np
from PIL import Image
import torch
from torch import nn
from torch.utils.data import DataLoader, Dataset
from torchvision import models, transforms

MEAN = np.array([.485, .456, .406], dtype=np.float32)
STD = np.array([.229, .224, .225], dtype=np.float32)


def preprocess(image):
    """Pixel-centre bilinear resize (no antialias), short edge 256, centre 224."""
    a = np.asarray(image.convert('RGB'), dtype=np.float32)
    h, w = a.shape[:2]
    nw, nh = (256, h * 256 // w) if w <= h else (w * 256 // h, 256)
    x = np.clip((np.arange(224) + (nw-224)//2 + .5)*w/nw-.5, 0, w-1)
    y = np.clip((np.arange(224) + (nh-224)//2 + .5)*h/nh-.5, 0, h-1)
    xi, yi = x.astype(int), y.astype(int)
    dx, dy = (x-xi)[None, :, None], (y-yi)[:, None, None]
    top = a[yi[:, None], xi] * (1-dx) + a[yi[:, None], np.minimum(xi+1,w-1)]*dx
    bot = a[np.minimum(yi+1,h-1)[:, None], xi]*(1-dx) + a[np.minimum(yi+1,h-1)[:, None], np.minimum(xi+1,w-1)]*dx
    rgb = np.floor(top*(1-dy)+bot*dy+.5).astype(np.float32)
    return ((rgb / 255 - MEAN) / STD).astype(np.float32)


class Birds(Dataset):
    def __init__(self, root, rows, augment=False):
        self.root, self.rows = root, rows
        self.transform = transforms.Compose([
            transforms.RandomResizedCrop(224, scale=(.65, 1.0)),
            transforms.RandomHorizontalFlip(), transforms.ToTensor(),
            transforms.Normalize(MEAN.tolist(), STD.tolist())]) if augment else None

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, index):
        row = self.rows[index]
        with Image.open(self.root / 'images' / row['path']) as im:
            im = im.convert('RGB')
            x = self.transform(im) if self.transform else torch.from_numpy(preprocess(im).transpose(2,0,1))
        return x, row['label']


def make_split(root):
    def table(name):
        return dict(line.split(' ', 1) for line in (root/name).read_text().splitlines())
    images, labels, official = table('images.txt'), table('image_class_labels.txt'), table('train_test_split.txt')
    classes = table('classes.txt')
    rng = random.Random(42)
    split = {k: [] for k in ('train', 'val', 'test')}
    for label in range(20):
        rows = [{'id': int(i), 'path': p, 'label': label} for i,p in images.items() if int(labels[i]) == label+1]
        train = [r for r in rows if official[str(r['id'])]=='1']
        rng.shuffle(train)
        n = round(len(train)*.2)
        split['val'].extend(train[:n]); split['train'].extend(train[n:])
        split['test'].extend(r for r in rows if official[str(r['id'])]=='0')
    return split, [classes[str(i)].split('.',1)[1].replace('_',' ') for i in range(1,21)]


def evaluate(model, loader):
    model.eval(); logits, labels = [], []
    with torch.inference_mode():
        for x,y in loader:
            logits.append(model(x)); labels.append(y)
    scores, truth = torch.cat(logits), torch.cat(labels)
    rank = scores.argsort(descending=True, dim=1)
    cm = torch.zeros(20,20,dtype=torch.int64)
    for t,p in zip(truth,rank[:,0]): cm[t,p] += 1
    return {'top1': (rank[:,0]==truth).float().mean().item(),
            'top5': (rank[:,:5]==truth[:,None]).any(1).float().mean().item(),
            'count': len(truth), 'confusion_matrix': cm.tolist(),
            'per_class_accuracy': (cm.diag()/cm.sum(1).clamp(min=1)).tolist()}, scores


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--epochs',type=int,default=30); p.add_argument('--threads',type=int,default=6)
    p.add_argument('--export-only',action='store_true')
    args=p.parse_args(); out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    torch.set_num_threads(args.threads); torch.manual_seed(42); random.seed(42); np.random.seed(42)
    torch.use_deterministic_algorithms(True)
    split, classes=make_split(args.data)
    (out/'split.json').write_text(json.dumps(split,indent=2)+'\n')
    (out/'labels.txt').write_text('\n'.join(classes)+'\n')
    loaders={k:DataLoader(Birds(args.data,v,k=='train'),batch_size=32,shuffle=k=='train',num_workers=4) for k,v in split.items()}
    model=models.mobilenet_v3_small(weights=None if args.export_only else models.MobileNet_V3_Small_Weights.IMAGENET1K_V1)
    model.classifier[3]=nn.Linear(model.classifier[3].in_features,20)
    history=[]; best=-1; stale=0
    if not args.export_only:
        for stage, epochs, lr in [('head',5,.001),('finetune',args.epochs,.0001)]:
            for param in model.parameters(): param.requires_grad_(stage=='finetune')
            for param in model.classifier.parameters(): param.requires_grad_(True)
            opt=torch.optim.AdamW(filter(lambda v:v.requires_grad,model.parameters()),lr=lr)
            for epoch in range(epochs):
                start=time.monotonic(); model.train()
                if stage=='head': model.features.eval()
                loss_sum=0
                for x,y in loaders['train']:
                    opt.zero_grad(); loss=nn.functional.cross_entropy(model(x),y); loss.backward(); opt.step()
                    loss_sum+=loss.item()*len(y)
                val,_=evaluate(model,loaders['val'])
                row={'stage':stage,'epoch':epoch+1,'loss':loss_sum/len(split['train']),'val_top1':val['top1'],'seconds':time.monotonic()-start}
                history.append(row); print(json.dumps(row),flush=True)
                (out/'history.json').write_text(json.dumps(history,indent=2)+'\n')
                if val['top1']>best:
                    best=val['top1']; stale=0; torch.save(model.state_dict(),out/'best.pt')
                elif stage=='finetune': stale+=1
                if stage=='finetune' and stale>=7: break
        (out/'packages.txt').write_text(subprocess.check_output([__import__('sys').executable,'-m','pip','freeze'],text=True))
    model.load_state_dict(torch.load(out/'best.pt',weights_only=True)); model.eval()
    metrics, scores=evaluate(model,loaders['test'])
    metrics['target_top1']=.8; metrics['accuracy_pass']=metrics['top1']>=.8
    metrics['pretraining_overlap_warning']='CUB may overlap ImageNet pretraining; held out from this fine-tuning only.'
    (out/'float-metrics.json').write_text(json.dumps(metrics,indent=2)+'\n')
    np.save(out/'test-logits.npy',scores.numpy())
    sample=next(iter(loaders['val']))[0][:1]
    torch.onnx.export(model,sample,out/'bird.onnx',input_names=['images'],output_names=['logits'],opset_version=17,dynamo=False)
    import onnxruntime as ort
    session=ort.InferenceSession(str(out/'bird.onnx'),providers=['CPUExecutionProvider'])
    errors=[]
    with torch.inference_mode():
        for x,_ in loaders['val']:
            for item in x[:2]:
                inp=item[None].numpy(); actual=session.run(None,{'images':inp})[0]; expected=model(item[None]).numpy()
                np.testing.assert_allclose(actual,expected,rtol=1e-4,atol=1e-4); errors.append(float(np.max(np.abs(actual-expected))))
    (out/'onnx-check.json').write_text(json.dumps({'max_abs_error':max(errors),'samples':len(errors),'passed':True},indent=2)+'\n')
    calibration=out/'calibration'; calibration.mkdir(exist_ok=True); entries=[]
    for label in range(20):
        for row in [r for r in split['train'] if r['label']==label][:10]:
            dest=calibration/f"{row['id']}.raw"
            with Image.open(args.data/'images'/row['path']) as im: preprocess(im).tofile(dest)
            entries.append(str(dest))
    (out/'calibration.txt').write_text('\n'.join(entries)+'\n')
    test_dir=out/'test-images';test_dir.mkdir(exist_ok=True); test_rows=[]
    for row in split['test']:
        name=f"{row['id']:05d}.png"
        with Image.open(args.data/'images'/row['path']) as im: im.convert('RGB').save(test_dir/name)
        test_rows.append(f"{row['label']}\t{name}")
    (test_dir/'test.tsv').write_text('\n'.join(test_rows)+'\n')
    files=['best.pt','bird.onnx','labels.txt','split.json']
    (out/'training-manifest.json').write_text(json.dumps({'seed':42,'classes':20,'architecture':'mobilenet_v3_small','preprocessing':'RGB short edge 256, pixel-centre bilinear no antialias rounded uint8, center 224, ImageNet normalization','sha256':{n:hashlib.sha256((out/n).read_bytes()).hexdigest() for n in files}},indent=2)+'\n')
    print(json.dumps(metrics),flush=True)

if __name__=='__main__': main()
