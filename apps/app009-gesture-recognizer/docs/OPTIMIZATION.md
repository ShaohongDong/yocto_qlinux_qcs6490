# Validation results and paused accuracy improvement

Updated: 2026-09-16. **Optimization is paused by user request. Do not resume
training, cache generation or board deployment without a new instruction.**
This commit preserves the working application and unfinished experiment code;
it does not claim the accuracy improvement plan has been completed.

## Deployed baseline: execution passed, accuracy failed

The independent app009 GUI/CLI supports eleven IPN Hand dynamic gestures plus
background, continuous file videos, and still-image preview only. Both neural
networks execute on QNN HTP; the deployed validation version is
`0.1.0-validation.1`, under `/var/lib/q6a-gesture-recognizer/current`.
Launch with `q6a-gesture-recognizer --gui`. The quality warning remains visible.

The original experiment trained six candidates on the PC GTX 1050 Ti 4GB.
Validation selected the regularized 32-frame spatial MobileNet/relative-TCN
candidate, with 10 Hz sampling, threshold 0.7 and release 0.9 seconds.
The local seed-42 split has 30/10/10 subject groups and 120/40/40 videos;
it is not the official IPN benchmark protocol. Original JPEG sequences were
verified and reconstructed as shared PC/board H.264 CRF10 videos (lossy).

| Historical 40-video test | PyTorch / ONNX | Q6A HTP | Required |
|---|---:|---:|---:|
| Clip Top-1 | 27.27% | 27.05% | >=80% |
| Clip macro F1 | 32.88% | 31.58% | >=75% |
| Continuous-event macro F1 | 16.51% | 16.30% | >=70% |
| Background false events/min | 2.768 | 2.742 | <=2 |

All absolute accuracy gates failed. On the historical test set, maximum HTP
metric degradation was 1.296 percentage points, within the 2-point gate.
Separately, validation event F1 lost 2.307 points after quantization and failed
that gate; the test result does not erase this failure.

The deployed baseline decoded 163374 test frames, sampled 54473 times and had
zero dropped frames or timestamp resets. Batch throughput was 153.64 samples/s.
A 600.05-second GUI soak produced 5995 samples (9.991 Hz), zero drops/resets,
and peak RSS 85832 KiB. Stop/restart, H.264/H.265 switching, image preview,
five intentional error cases and orderly teardown passed. There were no new
error-priority kernel records or gesture coredumps during that acceptance run;
the board boot ID remained unchanged. No OS update, flash or reboot was done.

These results belong to the **previous deployed binary**, SHA256
`9fd1a025ada4a21aa8805fbe568465cb3b10e6fbf95f6c3b3b4e31aeb4e92fff`.
Its Yocto recipe completed all 6696 tasks. The current paused source includes
subsequent changes and must not inherit that binary's full board acceptance.

Evidence is retained outside Git in `artifacts/gesture-recognizer/`:
`ACCEPTANCE.md`, `final-summary.json`, `recipe-provenance.json`,
`board-test/`, `board-delivery/`, and the versioned validation bundle.
Weights, datasets, caches and binaries are deliberately not committed.

## Approved improvement plan

- Keep all eleven classes and GUI/CLI file inputs; no camera or OS deployment.
- Use the current PC, with a 24-hour experiment budget and the final six hours
  reserved for conversion and board validation. Paused time does not consume it.
- Permit public pretrained hand models and real 20 Hz sampling. All deployed
  neural networks must execute on HTP; no CPU inference fallback.
- Compare four branches: full-frame absolute features, hand geometry/trajectory,
  hand ROI appearance, and ROI/geometry fusion. Compare 32 frames first, then
  64 frames for the strongest two branches; repeat the selected configuration
  with seed 123 after the primary seed 42 run.
- Balance persons and action instances; target roughly half background samples,
  mine difficult backgrounds only from training, supervise causal sequences,
  and later finetune the last three active ROI encoder modules.
- Select model and temporal confirmation settings using validation only.
  Stage goals are Top-1 >=50%, event macro F1 >=35%, false events <=2/min.
  These do not replace final acceptance gates.
- Cap new caches at 8 GiB and preserve at least 5 GiB of free disk space;
  do not delete the baseline's data or results.
- The original test set is now a historical regression set, not a fresh blind
  test. Arrange a later independent set with at least ten new people,
  ten repetitions per class/person and five minutes of background/person.
  Freeze the candidate before examining its results. Until all original gates
  and runtime checks pass, retain the unaccepted quality label.

## Completed before pause

1. Preserved the baseline source, selection, manifests and result summary in
   `artifacts/gesture-recognizer/improvement/`.
2. Audited current failure patterns: 167/440 historical test actions classified
   as background; one-/two-finger click event F1 was zero. A ground-truth-label
   oracle through the old decoder scored validation event F1 95.86%, suggesting
   classification is the primary bottleneck rather than event confirmation.
3. Added multi-output QNN support while preserving the legacy one-output API.
   `qnn-multi-probe` validates actual named tensor outputs and captures HTP events.
4. Downloaded official Lite palm/hand-landmark models and converted them to
   W8A16, SoC35/v68 using QAIRT 2.47. Both ran on Q6A HTP with positive
   accelerator profile events. On 64 training calibration samples, presence
   decisions agreed with TFLite; palm-center P95 error was 1.67 pixels and hand
   landmark P95 error was 2.93 pixels in their model input coordinates.
   This is an operator/quantization diagnostic, **not validation accuracy**.
5. Added the Python hand frontend, original-coordinate geometry/velocity,
   real-frame 20 Hz sampling, bounded cache generation, absolute/difference
   causal TCN candidates, balanced head-training and validation search code.
6. At suspension, 36/160 train/validation videos had complete hand observations
   and 8/160 had complete visual features. Partial video files also exist;
   only completion-marked directories are reusable after identity checks.

Pretrained model references (not vendored):

| Model | Official source | SHA256 |
|---|---|---|
| Palm Lite | [Google model](https://storage.googleapis.com/mediapipe-assets/palm_detection_lite.tflite) | `e9a4aaddf90dda56a87235303cf00e4c2d3fb28725f68fd88772997dac905c18` |
| Hand Landmark Lite | [Google model](https://storage.googleapis.com/mediapipe-assets/hand_landmark_lite.tflite) | `d7fde8ac11f8ce03f8663775bfc323f4fc9f2a38062b4f4efa142874ef5b2a48` |

Geometry follows the documented
[MediaPipe Hands graph configuration](https://github.com/google-ai-edge/mediapipe/tree/master/mediapipe/modules/hand_landmark).
Retain the upstream model licensing and attribution when packaging; see the
[official model card](https://storage.googleapis.com/mediapipe-assets/Model%20Card%20Hand%20Tracking%20%28Lite_Full%29%20with%20Fairness%20Oct%202021.pdf).
TFLite is a PC reference only; it is not a board fallback.

## Unfinished work and safe resumption

**No v2 gesture training run has started and no improved model was selected or
deployed.** The C++ hand frontend is a development header, not wired into the
application. Its syntax check is not a Python/C++ numerical parity test.
The application still loads the legacy 10 Hz, 576-feature pipeline.

Remaining implementation includes frontend numerical parity, end-to-end v2
runtime integration, versioned model contracts, per-stage reports, ROI joint
finetuning, the bounded experiment scheduler, v2 export/calibration/packaging,
full validation/regression comparison, and the 20 Hz board soak. The current
head-training script alone does not implement the whole planned experiment.
Multi-output raw probes do not establish continuous hand-tracking accuracy or
20 Hz end-to-end throughput.

Local resumption evidence is in `improvement/paused.json`, `budget.json`,
`paused-source/`, `paused-python-ast.json`, cache identity files and logs.
The two experiment process groups were suspended with SIGSTOP, retaining RAM
and GPU allocation. They are not scheduled to resume automatically. Numeric
process IDs may be reused: verify live command lines, process groups and source
identity before any signal; never blindly reuse IDs from a document.

Commit preparation formatted Python without changing its AST, as verified
against `paused-python-ast.json`. Text hashes therefore differ from the source
loaded by the suspended workers. Do not silently overwrite cache fingerprints
or mix resumed old-code output with a new run. On a later authorized resume,
verify the saved source/AST and model hashes, then explicitly migrate proven
format-only cache identity or restart from the matching saved source. If live
processes no longer exist, preserve completed videos and regenerate incomplete
ones after those checks. Rebase the time budget to exclude the pause.

PC dependencies are split between `requirements-training.txt` and optional
`requirements-improvement.txt` (Python 3.11). The separate QAIRT Python 3.12
converter environment additionally used decorator, attrs, cloudpickle, psutil,
typing_extensions, scipy, tornado, pytest and tflite 2.18.0. LLVM14 was extracted
under the experiment's `host-libs/`; it was not installed over system LLVM.
Exact converter/probe commands and logs remain in the experiment artifacts.

## Checks for this paused-source commit

- `cmake --build artifacts/gesture-recognizer/host -j4`: passed.
- `bash artifacts/gesture-recognizer/build-cross.sh`: ARM64 build passed.
- `ctest --test-dir artifacts/gesture-recognizer/host --output-on-failure`
  with the existing host dependency library path: 3/3 passed.
- `train-env/bin/python tests/test_pipeline.py` and `tests/test_hands.py`
  from this app (using the artifact virtual environment): 7/7 each passed.
- `tests/test_contract.py --probe <host>/contract-probe`: legacy Python/C++
  preprocessing and event contracts passed.
- C++17 `-Wall -Wextra -Werror -fsyntax-only` including `src/hand.hpp`: passed.
- `scripts/qcom-app validate --app app009-gesture-recognizer --machine
  radxa-dragon-q6a`: passed.

These focused checks do not include a new full Yocto recipe build or a new
board deployment/soak of the current source. Optimization processes remained
suspended throughout commit preparation.
