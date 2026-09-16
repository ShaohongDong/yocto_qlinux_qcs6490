#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Download a reproducible COCO pose subset, without the full image archives."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import random
import shutil
import time
import urllib.request
import zipfile

BASE = "https://s3.amazonaws.com/images.cocodataset.org"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(url, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        return
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=90) as response:
                with path.with_suffix(path.suffix + ".part").open("wb") as dest:
                    shutil.copyfileobj(response, dest)
            path.with_suffix(path.suffix + ".part").replace(path)
            return
        except Exception:
            if attempt == 3:
                raise
            time.sleep(2 ** attempt)


def choose(annotations, count, seed):
    eligible = sorted({a["image_id"] for a in annotations
                       if not a.get("iscrowd", 0) and a.get("num_keypoints", 0) > 0})
    if len(eligible) < count:
        raise ValueError("Not enough images with annotated human keypoints")
    random.Random(seed).shuffle(eligible)
    return eligible[:count]


def label(annotation, width, height):
    x, y, w, h = annotation["bbox"]
    points = annotation["keypoints"]
    if len(points) != 51 or w <= 0 or h <= 0:
        raise ValueError("Invalid COCO pose annotation")
    values = [0, (x + w / 2) / width, (y + h / 2) / height, w / width, h / height]
    for i in range(0, 51, 3):
        values.extend([points[i] / width, points[i + 1] / height, points[i + 2]])
    return " ".join(str(v) if isinstance(v, int) else f"{v:.8f}" for v in values)


def trainable(annotation):
    # A non-crowd person with zero labeled keypoints is still a positive box.
    # Dropping it incorrectly teaches the detector that this person is background.
    return (not annotation.get("iscrowd", 0)
            and annotation["bbox"][2] > 0 and annotation["bbox"][3] > 0
            and len(annotation.get("keypoints", [])) == 51)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=24)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    archive = root / "annotations_trainval2017.zip"
    download(BASE + "/annotations/annotations_trainval2017.zip", archive)
    # Fixed archive MD5 matches the official S3 ETag (single-part object).
    with archive.open("rb") as stream:
        if hashlib.file_digest(stream, "md5").hexdigest() != "f4bbac642086de4f52a3fdda2de5fa2c":
            raise ValueError("COCO annotation archive checksum mismatch")
    with zipfile.ZipFile(archive) as source:
        train = json.loads(source.read("annotations/person_keypoints_train2017.json"))
        test = json.loads(source.read("annotations/person_keypoints_val2017.json"))
    selected = choose(train["annotations"], 5500, 42)
    splits = {"train": selected[:5000], "val": selected[5000:],
              "test": choose(test["annotations"], 500, 42)}
    existing = root / "split.json"
    manifest = {"seed": 42, "ids": splits, "annotations_sha256": digest(archive)}
    if existing.exists() and json.loads(existing.read_text()) != manifest:
        raise ValueError("Existing split differs; use a new output directory")
    existing.write_text(json.dumps(manifest, indent=2) + "\n")
    rows = []
    for split, ids in splits.items():
        data = test if split == "test" else train
        ids = set(ids)
        images = {im["id"]: im for im in data["images"] if im["id"] in ids}
        annotations = [a for a in data["annotations"] if a["image_id"] in ids]
        grouped = {i: [] for i in ids}
        for a in annotations:
            if trainable(a):
                grouped[a["image_id"]].append(a)
        (root / "labels" / split).mkdir(parents=True, exist_ok=True)
        # Preserve original crowd/visibility metadata for COCO evaluation.
        (root / f"{split}-coco.json").write_text(json.dumps({
            **{k: data[k] for k in ("info", "licenses", "categories")},
            "images": list(images.values()), "annotations": annotations}))
        for image_id in sorted(ids):
            im = images[image_id]
            target = root / "images" / split / im["file_name"]
            source_split = "val2017" if split == "test" else "train2017"
            url = f"{BASE}/{source_split}/{im['file_name']}"
            rows.append((url, target, im))
            text = "\n".join(label(a, im["width"], im["height"]) for a in grouped[image_id])
            (root / "labels" / split / (target.stem + ".txt")).write_text(text + "\n")

    def fetch(row):
        url, target, im = row
        download(url, target)
        from PIL import Image
        with Image.open(target) as image:
            if image.size != (im["width"], im["height"]):
                raise ValueError(f"Image dimensions differ: {target}")
            image.verify()
        return {"path": str(target.relative_to(root)), "id": im["id"],
                "url": url, "license": im["license"], "sha256": digest(target)}

    hashes = []
    if not 1 <= args.workers <= 32:
        parser.error("workers must be 1..32")
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for record in pool.map(fetch, rows):
            hashes.append(record)
            if len(hashes) % 100 == 0:
                print(f"Verified {len(hashes)}/{len(rows)} images", flush=True)
    (root / "images.json").write_text(json.dumps(hashes, indent=2) + "\n")
    (root / "labels-policy.json").write_text(json.dumps({
        "version": 2, "policy": "All non-crowd valid person boxes, including zero-keypoint instances",
        "labels_sha256": {str(p.relative_to(root)): digest(p)
                          for p in sorted((root / "labels").rglob("*.txt"))}
    }, indent=2) + "\n")
    (root / "pose.yaml").write_text(
        f"path: {json.dumps(str(root))}\ntrain: images/train\nval: images/val\ntest: images/test\n"
        "names: {0: person}\nkpt_shape: [17, 3]\n"
        "flip_idx: [0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15]\n")
    print(root / "pose.yaml")


if __name__ == "__main__":
    main()
