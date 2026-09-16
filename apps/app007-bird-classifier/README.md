# Q6A Bird Classifier

Independent C++/GTK3 image classifier, QNN HTP only, with CLI and GUI. Classes are
CUB-200-2011 official IDs 1–20, displayed with the official English names.
Training fine-tunes ImageNet MobileNetV3-Small on the local PC. No camera,
object detection, automatic system update or CPU inference fallback is included.

## Dataset and training

Download `CUB_200_2011.tgz` from https://data.caltech.edu/records/65de6-vp158 .
The archive's official MD5 is checked before extracting the first 20 classes.
CUB use is restricted to non-commercial research/education. CUB images may
also appear in ImageNet; test results are held out from this fine-tuning, not
guaranteed held out from pretraining. See https://www.vision.caltech.edu/datasets/cub_200_2011/ .

```sh
python3 scripts/prepare-data.py /path/CUB_200_2011.tgz /path/data
python3 scripts/train.py --data /path/data/CUB_200_2011 --output /path/model
```

Use a dedicated environment with torch, torchvision, Pillow, numpy, onnx,
onnxruntime. Pin actual versions using the generated `packages.txt`. Defaults:
seed 42, official train/test split, stratified 20% validation from training,
batch 32, AdamW, five head epochs at 1e-3, up to 30 full epochs at 1e-4,
seven-epoch validation early stopping. Best checkpoint selected only by validation.
The export uses ONNX opset 17 and checks float output against PyTorch.
Calibration uses ten **training** images per class. Test PNGs preserve decoded
pixels so PC and board decoder differences do not affect acceptance comparisons.

Preprocessing: RGB, short edge 256, pixel-centre bilinear without antialiasing,
round to uint8, integer centre crop 224, normalize with ImageNet mean/std.
Training augmentation uses random resized crop and horizontal flip.
`split.json`, weights, ONNX, calibration, test set, metrics and dependency list
are generated outside Git. `--export-only` reuses the existing best checkpoint.

## Build and package

From the checkout root, activate the eSDK and run:

```sh
scripts/qcom-app validate --app app007-bird-classifier --machine radxa-dragon-q6a
scripts/qcom-app build --app app007-bird-classifier --machine radxa-dragon-q6a
```

Alternatively CMake can use an existing Yocto toolchain/sysroot with GTK3 and
QAIRT headers. Host CTest/`--self-test` cover software only.
Use `scripts/convert.py --help` for the QAIRT conversion and `scripts/package.py
--help` for the versioned bundle. Deploy the **context/bird.bin**, not the
converter's intermediate weight archive. Convolution weights use per-channel 8-bit quantization; activations use 16 bits.
Per-tensor convolution quantization severely degraded this model in diagnostics.
The converter also detects near-zero ReLU ranges that cannot represent the next
convolution's int32 biases. It expands only those ranges, deriving the minimum
safe power-of-two bound from trained biases and calibration encodings, with 2x
headroom. This model needs one bound of 32 instead of 1e-4; test data do not
participate in that calculation. Details are saved in `quantization-audit.json`.
The package includes matched Q6A
DSP shell libraries and checksums. Install on the board with `./install`.
Application root: `/var/lib/q6a-bird-classifier`; entry: `q6a-bird-classifier`.
`./uninstall` restores the previous application version, retaining evidence.
The current model supports only 20 species; softmax is not calibrated confidence
and low probability (<50%) is only an uncertainty hint, not a non-bird detector.

## Run and verify

```sh
q6a-bird-classifier --gui
q6a-bird-classifier --input bird.png --report result.json
q6a-bird-classifier --batch test-images/test.tsv --report accuracy.json
q6a-bird-classifier --input bird.png --warmup 10 --repeat 100 --report timing.json
```

For SSH GUI launches, use the actual board session environment (X11 or Wayland).
In non-login SSH shells use `/usr/local/bin/q6a-bird-classifier` explicitly.
Batch format: zero-based class ID, TAB, image path relative to TSV directory.
JSON includes Top-5, all logits, confusion matrix, model SHA256, timing,
peak RSS, and QNN profiling. `status=PASS` means execution succeeded; accuracy
acceptance is separately evaluated against Top-1 >=80% and <=2 percentage-point
loss versus floating point. `--dump-input` and `--dump-output` write float32
NHWC input/logits; use a single image when collecting comparison evidence.
Hardware proof requires accelerator execution events, not just backend names.
Report NPU inference latency separately from end-to-end image processing.
