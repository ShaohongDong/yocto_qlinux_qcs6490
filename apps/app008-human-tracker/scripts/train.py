#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fine-tune YOLOv8n-pose on CUDA; export the selected model and calibration."""
import argparse
import functools
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

CHECKPOINT_SHA256 = "c6fa93dd1ee4a2c18c900a45c1d864a1c6f7aba75d84f91648a30b7fb641d212"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--weights", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--export-only", action="store_true")
    parser.add_argument("--batch", type=int, choices=(1, 2, 4), default=4)
    parser.add_argument("--epochs", type=int, default=30)
    parser.add_argument("--train-only", action="store_true",
                        help="Stop before export and final-test evaluation during model selection")
    parser.add_argument("--conservative", action="store_true",
                        help="Freeze backbone/neck and BN statistics; disable strong augmentations")
    parser.add_argument("--lr", type=float, default=.0001)
    parser.add_argument("--imgsz", type=int, choices=(416, 640), default=416)
    parser.add_argument("--skip-final-eval", action="store_true",
                        help="Export only; evaluate the frozen selected model separately")
    args = parser.parse_args()
    if not 1 <= args.epochs <= 30:
        parser.error("epochs must be in 1..30")
    if args.resume and args.export_only:
        parser.error("resume and export-only are mutually exclusive")
    if not 0 < args.lr <= .01 or (args.train_only and args.export_only):
        parser.error("lr must be in (0, .01]; train-only and export-only are mutually exclusive")
    root, data, weights = args.output.resolve(), args.data.resolve(), args.weights.resolve()
    root.mkdir(parents=True, exist_ok=True)
    # Official v8.3.0 model serialization contains modules; trust only the pinned asset
    # and checkpoints generated locally by this training pipeline.
    if sha256(weights) != CHECKPOINT_SHA256:
        raise ValueError("Official pretrained checkpoint checksum mismatch")
    os.environ["TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD"] = "1"
    os.environ["YOLO_CONFIG_DIR"] = str(root / "yolo-config")
    os.environ["CUBLAS_WORKSPACE_CONFIG"] = ":4096:8"
    import cv2
    import numpy as np
    import torch
    from ultralytics import YOLO
    from ultralytics.models.yolo.pose import PoseTrainer
    class PoseOnlyTrainer(PoseTrainer):
        def get_model(self, cfg=None, weights=None, verbose=True):
            # Rebuilding this v8 checkpoint from YAML under 8.3.0 changes
            # classification-head topology and silently drops pretrained tensors.
            # Preserve the verified checkpoint's full architecture instead.
            from copy import deepcopy
            if weights is None:
                raise RuntimeError("Fine-tuning requires pretrained model weights")
            model = deepcopy(weights).float().requires_grad_(True)
            if model.model[-1].nc != 1 or list(model.model[-1].kpt_shape) != [17, 3]:
                raise RuntimeError("Checkpoint is not the expected human pose model")
            print(f"Preserved complete checkpoint architecture: {len(model.state_dict())} tensors", flush=True)
            return model

        def validate(self):
            metrics = self.validator(self)
            fitness = metrics["metrics/mAP50-95(P)"]
            if self.best_fitness is None or self.best_fitness < fitness:
                self.best_fitness = fitness
            return metrics, fitness

        def preprocess_batch(self, batch):
            batch = super().preprocess_batch(batch)
            # The preset's freeze=22 is saved in last.pt and restored on resume.
            # Apply after model.train(), which runs again at every epoch start.
            if self.args.freeze == 22:
                for module in self.model.modules():
                    if isinstance(module, torch.nn.modules.batchnorm._BatchNorm):
                        module.eval()
            return batch
    torch.set_num_threads(6)
    torch.onnx.export = functools.partial(torch.onnx.export, dynamo=False)
    dataset = data / "pose.yaml"
    if not args.export_only and (not dataset.is_file() or not (data / "images.json").is_file()):
        raise RuntimeError("Complete prepare-data.py before training/export")
    export_model = weights
    if not args.export_only:
        subprocess.run([sys.executable, str(Path(__file__).with_name("check-gpu.py")),
                        "--report", str(root / "gpu-check.json")], check=True)
        if (root / "fit").exists() and not args.resume:
            raise RuntimeError("Training directory exists: use --resume or a new output")
        start = time.monotonic()
        last = root / "fit/weights/last.pt"
        if args.resume:
            if not last.is_file():
                raise RuntimeError("No resumable last.pt checkpoint")
            YOLO(str(last)).train(trainer=PoseOnlyTrainer, resume=True, device=0, workers=4)
        else:
            model = YOLO(str(weights))
            overrides = {}
            if args.conservative:
                overrides = dict(freeze=22, mosaic=0., close_mosaic=0, scale=0.,
                                 translate=0., hsv_h=0., hsv_s=0., hsv_v=0.,
                                 fliplr=0., weight_decay=.0001)
            model.train(trainer=PoseOnlyTrainer, data=str(dataset), imgsz=args.imgsz, batch=args.batch, nbs=16,
                        device=0, workers=4, epochs=args.epochs, patience=7,
                        optimizer="AdamW", lr0=args.lr, lrf=.1 if args.conservative else .01, amp=False,
                        warmup_bias_lr=args.lr if args.conservative else .001, warmup_epochs=1.0,
                        seed=42, deterministic=True, pretrained=True,
                        project=str(root), name="fit", exist_ok=False,
                        save=True, plots=True, cache=False, **overrides)
        (root / "training-session.json").write_text(json.dumps({
            "seconds": time.monotonic() - start, "resumed": args.resume,
            "batch": args.batch, "effective_batch": 16, "imgsz": args.imgsz,
            "conservative": args.conservative, "lr": args.lr}, indent=2) + "\n")
        export_model = root / "fit/weights/best.pt"
        if args.train_only:
            print(f"Model selection checkpoint: {export_model}; final test not evaluated")
            return
    elif (root / "fit/weights/best.pt").is_file():
        export_model = root / "fit/weights/best.pt"

    model = YOLO(str(export_model))
    exported = Path(model.export(format="onnx", imgsz=args.imgsz, opset=17,
                                 simplify=False, dynamic=False, device="cpu"))
    import shutil
    shutil.copy2(exported, root / "pose.onnx")
    license_path = weights.parent / "LICENSE-model"
    if not license_path.is_file():
        raise RuntimeError("Keep the official Ultralytics license beside the verified weights")
    if license_path.resolve() != (root / "LICENSE-model").resolve():
        shutil.copy2(license_path, root / "LICENSE-model")
    calibration = root / "calibration"
    calibration.mkdir(exist_ok=True)
    paths = []
    calibration_ids = sorted(json.loads((data / "split.json").read_text())["ids"]["train"])[:128]
    for image_id in calibration_ids:
        path = data / "images/train" / f"{image_id:012d}.jpg"
        if not path.is_file():
            raise RuntimeError(f"Missing fixed calibration image: {path}")
        image = cv2.cvtColor(cv2.imread(str(path)), cv2.COLOR_BGR2RGB)
        from pose_common import preprocess
        raw = calibration / (path.stem + ".raw")
        preprocess(image, args.imgsz).tofile(raw)
        paths.append(str(raw))
    if len(paths) != 128:
        raise RuntimeError("Calibration requires 128 training images")
    (root / "calibration.txt").write_text("\n".join(paths) + "\n")
    # Compare matching inputs before evaluating postprocessed AP on a held-out set.
    import onnxruntime as ort
    session = ort.InferenceSession(str(root / "pose.onnx"), providers=["CPUExecutionProvider"])
    raw = np.fromfile(paths[0], np.float32).reshape(1, args.imgsz, args.imgsz, 3).transpose(0, 3, 1, 2)
    model.model.cpu().eval()
    with torch.inference_mode():
        output = model.model(torch.from_numpy(raw))
        expected = (output[0] if isinstance(output, (tuple, list)) else output).numpy()
    actual = session.run(None, {session.get_inputs()[0].name: raw})[0]
    error = float(np.max(np.abs(actual - expected)))
    np.testing.assert_allclose(actual, expected, atol=.002, rtol=.0002)
    manifest = {"schema_version": 1, "trained": export_model != weights,
                "model": "YOLOv8n-pose", "input": [1, 3, args.imgsz, args.imgsz],
                "keypoints": 17, "seed": 42, "export_max_abs_error": error,
                "pretrained_sha256": CHECKPOINT_SHA256,
                "checkpoint_sha256": sha256(export_model),
                "onnx_sha256": sha256(root / "pose.onnx"),
                "split_sha256": sha256(data / "split.json"),
                "labels_policy_sha256": sha256(data / "labels-policy.json") if (data / "labels-policy.json").exists() else None,
                "calibration": "128 sorted training images only",
                "evaluation_scope": "Held out from this fine-tuning, not guaranteed unseen in pretraining"}
    (root / "training-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (root / "packages.txt").write_text(subprocess.check_output([sys.executable, "-m", "pip", "freeze"], text=True))
    # Smoke exports intentionally stop here; they must never be labeled trained.
    if export_model == weights:
        print("Pretrained conversion smoke export only; no training performed")
        return
    if args.skip_final_eval:
        return
    metrics = {}
    device = 0 if torch.cuda.is_available() else "cpu"
    for name, checkpoint in (("baseline", weights), ("finetuned", export_model), ("onnx", root / "pose.onnx")):
        result = YOLO(str(checkpoint), task="pose").val(
            data=str(dataset), split="test", imgsz=args.imgsz, batch=1, rect=False,
            device="cpu" if name == "onnx" else device, workers=4,
            project=str(root), name="eval-" + name, plots=False, save_json=True)
        metrics[name] = {"pose_ap": float(result.pose.map), "pose_ap50": float(result.pose.map50),
                         "box_ap": float(result.box.map)}
    metrics["onnx_acceptance"] = metrics["finetuned"]["pose_ap"] - metrics["onnx"]["pose_ap"] <= .005
    (root / "accuracy.json").write_text(json.dumps(metrics, indent=2) + "\n")
    if not metrics["onnx_acceptance"]:
        raise RuntimeError("ONNX keypoint AP loss exceeds 0.5 percentage points")


if __name__ == "__main__":
    main()
