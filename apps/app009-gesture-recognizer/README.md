# Q6A Dynamic Gesture Recognizer

> **Status (2026-09-16): accuracy improvement is paused at the user's request.**
> The deployed `0.1.0-validation.1` build runs on Q6A but has **not passed accuracy
> acceptance**. The new hand-localization/20 Hz training code is an incomplete
> development snapshot, not the deployed pipeline. See
> [validation results and paused optimization handoff](docs/OPTIMIZATION.md).


Independent GTK3/CLI continuous-video recognition of the eleven IPN Hand dynamic
actions G01..G11, plus background. Image files are preview-only. No camera or
cross-person identity tracking is included. Both MobileNetV3-Small features and
causal temporal convolutions execute on QNN HTP; H.264/H.265 decoding uses the board V4L2 hardware decoder. CPU handles RGB
conversion, preprocessing, temporal buffers and event confirmation. No CPU inference fallback.

## Data and training

Install `requirements-training.txt` in a Python 3.11 environment; FFmpeg/ffprobe
must also be available. Run `check-gpu.py`
to verify actual forward/backward/optimizer operations on the GTX 1050 Ti.
The official IPN Hand RGB videos and annotations are CC-BY-4.0:
https://gibranbenitez.github.io/IPN_Hand/ . Retain attribution with distributed
samples. The backbone uses torchvision IMAGENET1K_V1 initialization; retain
torchvision BSD-3-Clause notices separately from dataset attribution.

`prepare-data.py --output <data>` downloads the official RGB frame archives, verifies all
200 frame sequences, reconstructs 30 FPS H.264 High/CRF10 BT.601 limited-range videos and freezes a seed-42 subject-grouped 60/20/20 split. The metadata's
first two video-name fields identify 50 groups of four recordings; consistency
with the original subject split is checked. This is a local protocol, not an
official benchmark submission. Static pointing and non-gesture intervals are
background. Original AVI files have frame-count mismatches (upstream issue 11)
and are not used. JPEG indices must exactly match annotations; non-image files
such as desktop.ini are excluded from metadata file counts. PC and board use
the same reconstructed video sources. CRF10 is high-quality lossy encoding,
not a claim of pixel-identical JPEG preservation; QP0 uses a profile unsupported
by the current hardware decoder. Do not change the split after training begins.

`cache-data.py --data <data> --output <cache>` stores lossless 224 RGB samples at
10 Hz, pretrained features and the frozen backbone prefix. The default
`--architecture regularized` retains six spatial regions and subtracts the first
feature vector in each temporal window. Channel dropout, causal speed perturbation
and feature gain augmentation reduce overfitting; background receives four times
the samples of each action class. Use the same architecture when caching/training. Finetuning
recomputes the active tail: one residual block for spatial variants, two blocks
for global/multiview variants. BatchNorm statistics remain fixed. Prefix
equivalence is checked. It
verifies complete decoding; allow about 50 GiB for all uncompressed caches. Dataset/model/cache files stay outside Git.
`train.py --data <data> --cache <cache> --output <run> --window 32` trains on CUDA.
Repeat in a separate directory with `--window 48`; select only by validation
continuous-event macro F1, preferring background false events <=2/minute.
`global` and `spatial` retain earlier baseline architectures; `multiview` adds a
fixed internal crop enlarged from the central/lower input region while preserving
the full-frame view. It still produces 576 features in one NPU graph. Checkpoints
record architecture; export/evaluation recreate the matching network. Historical
experiment resumes require their matching scripts and provenance.

Freeze candidates before test with `select.py --data <data> --runs <run32> <run48>
--output <selected>`. Never evaluate test performance while selecting models.

Full-frame preprocessing: pixel-centre bilinear resize without antialiasing,
round uint8, aspect-preserving 114 letterbox to 224x224, ImageNet normalization.
No horizontal flip or temporal reversal. Temporal input is the latest 32/48
feature vectors in chronological order. Four 128-channel causal convolutions
use kernel 3 and dilation 1/2/4/8 (32-frame candidate), or 1/2/4/16
(48-frame candidate), so the longer candidate actually sees longer history.
Both produce a 12-class output.

## Conversion and application

`export.py --checkpoint <best.pt> --output <model> --window 32` exports fixed
ONNX opset 17 and compares outputs. `--smoke` explicitly permits an untrained
temporal head solely for operator testing, never for accuracy acceptance.
Convert `features` first with `convert.py`, collect its quantized outputs on
training samples, then calibrate/convert `temporal` using those outputs.
Use QAIRT 2.47, SoC35/v68, W8A16, per-channel weights and 32-bit biases.
The converter derives any bias-range corrections from training calibration
and trained parameters only; its audit is retained.

The model directory contains features.bin and temporal.bin from the **context**
subdirectories, model/training manifests, labels/license and model.ini:

```ini
[gesture]
window=32
threshold=0.6
release=0.6
accuracy_accepted=false
```

The shipped thresholds are selected on validation, not tuned on test videos.
`accuracy_accepted` is a separate quality label, set by the final acceptance audit.
Absent/false values visibly identify an unaccepted validation model.
A candidate must be stable for three samples before activation. An event is
emitted once after background confirmation or a stable class switch. Trailing
unconfirmed actions are marked incomplete. File changes, loops and timestamp
discontinuities reset history. Repeated identical actions require release in
between; report failures on adjacent same-class actions instead of hiding them.

```sh
q6a-gesture-recognizer --gui
q6a-gesture-recognizer --input video.mp4 --events events.jsonl --report report.json
q6a-gesture-recognizer --batch videos.txt --events events.jsonl --predictions scores.jsonl --report report.json
q6a-gesture-recognizer --gui --input video.mp4 --duration 600 --report soak.json
```

Batch entries are relative to the list file. CLI processes all frames and samples
at 10 Hz; GUI uses realtime playback and may drop old decoder frames. `--exact`
disables GUI dropping/pacing. Event JSONL includes source, loop, class, start/end,
confirmation time, confidence and completion flag. Prediction JSONL records
source frame/stream-time and all probabilities. GStreamer segment offsets are
removed using stream-time conversion, including initial negative-DTS compensation.
Reports distinguish sampling,
decoding, NPU feature/temporal latency, memory and hardware execution events.
`PASS` is execution success, not accuracy acceptance. Softmax is an uncalibrated
score. The initial history collection interval is not inference latency.

## Build and acceptance

Use `scripts/qcom-app validate/build --app app009-gesture-recognizer --machine
radxa-dragon-q6a` from the repository root after SDK setup. Build only the
application. Versioned install/uninstall scripts manage this application under
/var/lib/q6a-gesture-recognizer without replacing other apps or the OS.

Acceptance targets: eleven-class clip Top-1 >=80%, macro F1 >=75%, continuous
event macro F1 >=70%, background false events <=2/minute. Match same-class events
one-to-one at temporal IoU >=0.3; count duplicates as false positives. Report
per-class confusion, misses, false events and confirmation delay. Quantized
cascade loss must be <=2 percentage points relative to floating point, and
ONNX loss <=0.5 point. Calibration uses training data exclusively.

Require both graphs' actual HTP execution events, sustained 10 Hz sampling,
H.264/H.265, stop/restart/file switch, failure tests and a ten-minute GUI soak.
Do not relabel an unmet target as a pass. Reports, checkpoints, downloads,
packages and physical-board evidence belong in artifacts/gesture-recognizer/.

`evaluate.py` checks the frozen checkpoint identity and all expected samples for
Torch, ONNX or board predictions. `accept.py` compares the full held-out reports,
model hashes and HTP evidence, sets the quality label and writes the model manifest.
`package.py` rejects untrained or unaccepted models by default. Its explicit
`--development` option preserves failed accuracy status for an experimental board
bundle; it does not turn failed targets into acceptance.
