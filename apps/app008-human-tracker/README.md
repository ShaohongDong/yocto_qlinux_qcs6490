# Q6A Human Tracker

Independent C++17/GTK3 video-file application: 17 human body keypoints,
ByteTrack person IDs and motion trails. Pose inference runs only through QNN
HTP; CPU handles decoding, preprocessing, NMS, association and rendering.
Missing NPU/model/runtime is an error, never a CPU inference fallback.

This is application-only deployment on the existing Q6A system. No camera,
3D pose, action classification, cross-video identity, image rebuild or OTA.

## PC training

Use Python 3.11 in a dedicated environment outside Git, not an activated Yocto
SDK shell. Install torch 2.7.1 and torchvision 0.22.1 from the official cu118
index, then `scripts/requirements-train.txt`. For GTX 1050 Ti, validate the
actual CUDA forward/backward/optimizer test with `scripts/check-gpu.py`.
`nvidia-smi` alone is not training acceptance.

From the repository root (script paths below are under this application):

```sh
artifacts/human-tracker/train-env/bin/python -B apps/app008-human-tracker/scripts/check-gpu.py \
  --report artifacts/human-tracker/gpu/after-reboot.json
artifacts/human-tracker/train-env/bin/python -B apps/app008-human-tracker/scripts/prepare-data.py \
  --output artifacts/human-tracker/data
artifacts/human-tracker/train-env/bin/python -B apps/app008-human-tracker/scripts/train.py \
  --data artifacts/human-tracker/data --output artifacts/human-tracker/trained \
  --weights artifacts/human-tracker/model/yolov8n-pose.pt
```

Official weights: https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n-pose.pt .
The script verifies a pinned SHA256. Keep the Ultralytics v8.3.0 AGPL-3.0 license
beside the model as LICENSE-model. Model tooling/weights retain their original
license; this application's original C++/Python source and ByteTrack are MIT.
COCO and video images retain their individual source licenses.

Data: official COCO person-keypoint annotations, fixed seed 42; 5,000 train2017
images for training, 500 disjoint train2017 images for checkpoint selection,
and 500 val2017 images for final evaluation. The complete split, original
annotations, image source URLs/licenses and SHA256 values are saved outside Git.
The final set is held out from this fine-tuning, not necessarily pretraining.
Downloads resume completed files and validate all images.

Training: preserve the complete pretrained network topology (reconstruction
from YAML in Ultralytics 8.3.0 can change the classification head); FP32,
416x416, batch 4, nominal batch 16 with gradient accumulation, AdamW 1e-4,
30-epoch maximum, pose-AP checkpoint selection and patience 7. Use `--resume`
for last.pt. GPU OOM: use batch 2, then 1 in a new output directory, retaining
failed-run evidence. No automatic CPU training fallback.

For accuracy optimization, keep the original experiment and use a new data/output
directory. Label policy v2 preserves non-crowd person boxes even when all 17
keypoints are unannotated; these boxes must not be taught as background.
`labels-policy.json` records the policy and every label hash. The image split
is unchanged. Do not modify labels underneath an active training run.

Use `--conservative --lr 0.000005 --imgsz 640 --epochs 4 --train-only` for a
short head-only experiment: freeze backbone/neck, keep BatchNorm running
statistics fixed on every training batch, disable Mosaic/geometric/color/flip
augmentation and reduce learning rate. This is an explicit experiment, not a
guarantee of better accuracy. `--train-only` never exports or evaluates the final
test split. Compare candidates and the pretrained baseline using
`evaluate.py reference --split val --imgsz 640 --device cuda` followed by
`evaluate.py pose --split val`. Select and record the checkpoint before testing.
Use `--export-only --imgsz 640 --skip-final-eval` to export the frozen candidate,
then evaluate it separately on `--split test` with the same image size. Always
compare the pretrained model at the same resolution to separate training gains
from resolution gains. Final-test images were excluded from this fine-tuning;
they are not guaranteed unseen by the original pretrained model.

Use `--export-only` to export best.pt. If no trained checkpoint exists, it
exports the official weights with `trained=false` for conversion smoke only.
It may run before all image downloads complete if the fixed 128 training
calibration images already exist. Formal training requires the complete data.
Export uses fixed ONNX opset 17 and validates tensor agreement. Calibration
uses 128 training images only. No evaluation images affect quantization.

```sh
python3 -B apps/app008-human-tracker/scripts/convert.py \
  --sdk artifacts/ai-demo/sdk/qairt/2.47.0.260601 \
  --output artifacts/human-tracker/trained \
  --converter-python artifacts/ai-demo/converter-env/bin/python \
  --host-library-path /home/dsh/Desktop/qcom_sdk/artifacts/ai-demo/host-libs/root/usr/lib/llvm-18/lib
```

Use QAIRT 2.47.0.260601, SoC 35, HTP v68, W8A16, per-channel weights and
32-bit biases. Deploy **context/pose.bin**, never the intermediate pose.bin.
The generated manifest records whether the model was actually fine-tuned.

## Application behavior

Network input: RGB/255, 416x416 or 640x640 with 114 padding, read from the loaded
QNN context rather than selected by the user. Pixel-centre bilinear resize
without antialiasing, uint8 rounding and letterbox coordinate restoration are
shared with Python calibration/evaluation and checked numerically against C++.
Output is [1,56,3549] at 416 or [1,56,8400] at 640, or its channels-last
equivalent; mismatched shapes are rejected. NMS IoU is 0.7.

ByteTrack uses low detection cutoff 0.1, high association threshold 0.25,
new-track threshold 0.35, matching cost limit 0.8 and 30 source-frame lost
buffer. Predicted Kalman rectangles are refreshed before association; high-score
and tentative-track association use score-fused cost `1 - IoU * confidence`.
Low-score recovery keeps plain IoU cost. Both association stages propagate the original detection index so
keypoints cannot be assigned to a different nearby box. Skipped GUI frames
age tracks; long gaps expire tracks. Trails retain at most 90 observations.
IDs reset on file changes or loop restarts, paired with a loop number in JSONL.
Only currently matched tracks are drawn; lost-track positions are not presented
as measured poses. Still images and batch evaluation have no temporal IDs.

GUI: choose image/video, start/stop, skeletons, IDs, trails and confidence
visibility control. CLI processes every frame by default. GUI uses a two-frame
queue and may drop old frames; `--exact` disables dropping and real-time pacing.
H.264/H.265 decoding explicitly negotiates linear NV12 before RGB conversion.

```sh
q6a-human-tracker --gui
q6a-human-tracker --input video.mp4 --tracks tracks.jsonl --report result.json
q6a-human-tracker --gui --input video.mp4 --duration 600 --report soak.json
q6a-human-tracker --batch test-list.txt --confidence .001 --tracks poses.jsonl --report batch.json
```

Batch lists contain one image path per line, relative to the list directory.
JSONL contains source frame index, timestamp, loop, input path, dimensions,
boxes, per-keypoint coordinates/confidence and track IDs. Summary JSON includes
model SHA256, tracker revision/settings, model input side, decoded/processed/dropped counts, inference and pipeline p50/p95,
FPS, peak RSS and QNN profiling. Pipeline time includes sample wait, RGB copy,
preprocessing, inference and postprocessing; it excludes GUI presentation.
QNN inference time includes tensor quantization/dequantization; DSP execution
is separately reported in QNN profiling. `PASS` means execution succeeded;
accuracy acceptance is evaluated separately.

`--detections raw.jsonl` captures decoded detections before tracking, independently
of `--tracks`. Replay them on the PC with `scripts/replay-tracks.py --detections
raw.jsonl --replay <host-build>/tracker-replay --output replay.jsonl --fuse-score`.
The replay utility defaults to unfused association for explicit ablations; the
application enables score fusion. Freeze settings using independent validation
sequences (MOT17-02/10 here), then evaluate unchanged MOT17-05/09 test sequences.
Report IDF1, ID switches, fragments, misses and false positives together: fewer
IDs alone can hide a recall regression. The model and detection thresholds must
be identical when comparing association algorithms.

## Build, package and deploy

```sh
source ./environment-setup-armv8a-qcom-linux
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app validate --app app008-human-tracker --machine radxa-dragon-q6a
PYTHONDONTWRITEBYTECODE=1 scripts/qcom-app build --app app008-human-tracker --machine radxa-dragon-q6a
```

CMake can also use a matching existing Yocto toolchain/sysroot. Dependencies:
GTK3, GStreamer core/app/video/pbutils, QNN headers and header-only Eigen3.
Vendored ByteTrack origin/revision and local changes are recorded under
third_party/bytetrack/ORIGIN.md, with its license preserved in the install tree.

`scripts/package.py --help` describes versioned packaging. Supply the trained
model directory, CMake build, matched Q6A CDSP libraries, and an assets directory
with sample.png, sample.mp4 and sources.json. Packaging refuses untrained
weights unless `--allow-untrained` explicitly requests a development bundle.
The sample video is a real MOT17 sequence transcoded to H.264, not animation.

On the board, run the bundle's `install` as root. It verifies SHA256SUMS,
installs under /var/lib/q6a-human-tracker/releases, creates the CLI/desktop
entry and atomically changes current. `uninstall` restores the previous version
or removes this app's entry points, retaining evidence. Existing system
libraries, kernel, firmware and other applications are preserved.

## Validation

- Host CTest: pose geometry/layout, malformed values, two-stage association,
  occlusion recovery, source-frame gaps, reset and resizing.
- `tests/test_data.py`: split and annotation conversion.
- `tests/test_contract.py --probe <host-build>/pose-contract-probe`: exact Python/
  C++ preprocessing and decoding agreement; run with `--imgsz 416` and `--imgsz 640`.
- `scripts/evaluate.py reference` produces held-out float/ONNX predictions and
  lossless test PNGs; `pose` scores PC/board JSONL using the same COCO evaluator.
- ONNX pose AP loss <=0.5 percentage points; NPU loss <=2 points relative to the
  fine-tuned floating model. Also report the pretrained baseline without
  claiming fine-tuning must improve it.
- `evaluate.py mot` scores complete first-pass MOT17 videos with manual IDs,
  IoU >=0.5 and distractor exclusion. Frame counts must match; MOT17-09 preview
  coordinates scale by 2, MOT17-05 by 1. Report IDF1/switches/fragmentation as
  local sequence results, not an official benchmark submission.
- Real hardware requires accelerator execute events, H.264/H.265 video checks,
  GUI stop/restart/file switching, failure handling and a ten-minute soak.

Run-specific logs, models, metrics and screenshots belong in
artifacts/human-tracker/. Host tests do not establish board operation.
