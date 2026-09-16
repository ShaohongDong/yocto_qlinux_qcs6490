#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Download official IPN Hand, verify complete videos and freeze subject split."""

import argparse, csv, json, hashlib, random, tarfile, subprocess, concurrent.futures
from pathlib import Path
import gdown

IDS = [
    "17yn_1n3LrMHLVSCT4zAbN6sbGXKy1GlA",
    "1OBlvjl-Z0Wr6xXnLaCGGwFUEXJQLCkDw",
    "1YCDW763mlXQlfuydFHX_EHG_hyBWC48F",
    "1I90cK_4gyyQQRkLjFn2p90CCIPV4q41Y",
    "1SY_TEQX80MtjygS8RqASqRMDDPyiENlZ",
]


def sha(p):
    h = hashlib.sha256()
    with p.open("rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--no-download", action="store_true")
    a = p.parse_args()
    root = a.output.resolve()
    (root / "downloads").mkdir(parents=True, exist_ok=True)
    if not a.no_download:
        gdown.download_folder(
            "https://drive.google.com/drive/folders/1-mihJEIFoNDpfo1puF8xAMJz6PGVKsBD",
            output=str(root / "annotations"),
            quiet=True,
            remaining_ok=True,
        )
        for i, id in enumerate(IDS, 1):
            f = root / "downloads" / f"frames{i:02}.tgz"
            if not f.exists():
                gdown.download(id=id, output=str(f), quiet=False, resume=True)
    (root / "videos").mkdir(exist_ok=True)
    sources = []
    for i, id in enumerate(IDS, 1):
        f = root / "downloads" / f"frames{i:02}.tgz"
        sources.append(
            {
                "name": f.name,
                "url": "https://drive.google.com/file/d/" + id,
                "sha256": sha(f),
            }
        )
        with tarfile.open(f) as archive:
            archive.extractall(root, filter="data")
    meta = list(csv.DictReader((root / "annotations/metadata.csv").open()))
    ann = list(csv.DictReader((root / "annotations/Annot_List.txt").open()))
    groups = {}
    for row in meta:
        groups.setdefault("_".join(row["Video Name"].split("_")[:2]), []).append(row)
    assert (
        len(meta) == 200
        and len(groups) == 50
        and all(len(v) == 4 for v in groups.values())
    )
    # Group IDs reproduce the official 37/13 subject split without crossing sets.
    assert all(len({r["Set"] for r in v}) == 1 for v in groups.values())
    subjects = sorted(groups)
    random.Random(42).shuffle(subjects)
    mapping = {
        s: ("train" if i < 30 else "val" if i < 40 else "test")
        for i, s in enumerate(subjects)
    }

    def reconstruct(row):
        name = row["Video Name"]
        directory = root / "frames" / name
        frames = sorted(directory.glob("*.jpg"))
        expected = max(int(x["t_end"]) for x in ann if x["video"] == name)
        indices = sorted(int(f.stem.rsplit("_", 1)[1]) for f in frames)
        if indices != list(range(1, expected + 1)):
            raise RuntimeError("RGB frame/annotation mismatch: " + name)
        non_images = [
            f.name
            for f in directory.iterdir()
            if f.is_file() and f.suffix.lower() != ".jpg"
        ]
        if int(row["Frames"]) != expected + len(non_images):
            raise RuntimeError("Metadata file count mismatch: " + name)
        path = root / "videos" / (name + ".mp4")
        if not path.exists():
            temporary = path.with_suffix(".partial.mp4")
            subprocess.run(
                [
                    "ffmpeg",
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-framerate",
                    "30",
                    "-start_number",
                    "1",
                    "-i",
                    str(directory / (name + "_%06d.jpg")),
                    "-frames:v",
                    str(expected),
                    "-an",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "fast",
                    "-crf",
                    "10",
                    "-profile:v",
                    "high",
                    "-color_range",
                    "tv",
                    "-colorspace",
                    "smpte170m",
                    "-color_primaries",
                    "smpte170m",
                    "-color_trc",
                    "smpte170m",
                    "-pix_fmt",
                    "yuv420p",
                    "-threads",
                    "2",
                    "-y",
                    str(temporary),
                ],
                check=True,
            )
            temporary.replace(path)
        print("Reconstructed", name, expected, flush=True)
        return name, non_images

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        ignored_files = dict(pool.map(reconstruct, meta))
    videos = {}
    counts = {s: [0] * 12 for s in ["train", "val", "test"]}
    for subject, rows in groups.items():
        for row in rows:
            name = row["Video Name"]
            path = root / "videos" / (name + ".mp4")
            probe = json.loads(
                subprocess.check_output(
                    [
                        "ffprobe",
                        "-v",
                        "error",
                        "-select_streams",
                        "v:0",
                        "-show_entries",
                        "stream=nb_frames,width,height,avg_frame_rate",
                        "-of",
                        "json",
                        str(path),
                    ]
                )
            )["streams"][0]
            assert (
                int(probe["nb_frames"]) == int(row["Frames"]) - len(ignored_files[name])
                and probe["avg_frame_rate"] == "30/1"
            ), (name, probe)
            segments = []
            last = 0
            for item in [x for x in ann if x["video"] == name]:
                start = int(item["t_start"]) - 1
                end = int(item["t_end"])
                label = max(0, int(item["id"]) - 3)
                assert start == last and end > start and end <= int(row["Frames"]), (
                    name,
                    item,
                    last,
                )
                segments.append(
                    {
                        "start_frame": start,
                        "end_frame": end,
                        "label": label,
                        "original_label": item["label"],
                    }
                )
                last = end
                counts[mapping[subject]][label] += 1
            assert last == int(probe["nb_frames"])
            videos[name] = {
                "subject": subject,
                "split": mapping[subject],
                "path": str(path.relative_to(root)),
                "frames": last,
                "metadata_file_count": int(row["Frames"]),
                "ignored_non_image_files": ignored_files[name],
                "sha256": sha(path),
                "segments": segments,
            }
    for c in counts.values():
        assert min(c) > 0
    result = {
        "seed": 42,
        "protocol": "local subject grouped 60/20/20; not official benchmark split",
        "source_policy": "Official JPEG sequence, exact annotation frame order, 30 FPS H264 CRF10 High yuv420p BT601 limited reconstruction. Original AVI rejected for frame mismatch (upstream issue 11). RGB preprocessing reads these same reconstructed videos on PC and Q6A.",
        "subject_key": "first two underscore-separated metadata Video Name fields; 50 groups of 4; official split consistency checked",
        "subjects": mapping,
        "videos": videos,
        "segment_counts": counts,
        "sources": sources,
        "annotation_sha256": {
            f.name: sha(f) for f in (root / "annotations").glob("*.txt")
        },
        "license": "CC-BY-4.0",
        "source": "https://gibranbenitez.github.io/IPN_Hand/",
    }
    target = root / "split.json"
    if target.exists() and json.loads(target.read_text()) != result:
        raise RuntimeError("Refusing to overwrite a different frozen split")
    target.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(counts), flush=True)


if __name__ == "__main__":
    main()
