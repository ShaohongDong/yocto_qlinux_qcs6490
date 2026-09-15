#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproduce the QCS6490 model outside the source tree. See README for environments."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import urllib.request
import zipfile

ASSETS = {
    "LICENSE-model": ("https://raw.githubusercontent.com/ultralytics/ultralytics/v8.3.0/LICENSE", "0d96a4ff68ad6d4b6f1f30f713b18d5184912ba8dd389f86aa7710db079abcb0"),
    "yolov8n.pt": ("https://github.com/ultralytics/assets/releases/download/v8.3.0/yolov8n.pt", "f59b3d833e2ff32e194b5bb8e08d211dc7c5bdf144b90d2c8412c47ccfc83b36"),
    "bus.jpg": ("https://raw.githubusercontent.com/ultralytics/ultralytics/v8.3.0/ultralytics/assets/bus.jpg", "c02019c4979c191eb739ddd944445ef408dad5679acab6fd520ef9d434bfbc63"),
    "coco128.zip": ("https://github.com/ultralytics/assets/releases/download/v0.0.0/coco128.zip", "61e5e3028863d8ffc3b81d6a514603954889f0edd5e4b44c4ce60b2da99aeb8e"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--export-python", required=True, type=Path)
    parser.add_argument("--converter-python", required=True, type=Path)
    parser.add_argument("--host-library-path", default="")
    args = parser.parse_args()
    sdk, output = args.sdk.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    for name, (url, expected) in ASSETS.items():
        path = output / name
        if not path.exists():
            urllib.request.urlretrieve(url, path)
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise SystemExit(f"Checksum mismatch: {path}")
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", QNN_SDK_ROOT=str(sdk),
               PYTHONPATH=str(sdk / "lib/python"),
               LD_LIBRARY_PATH=str(sdk / "lib/x86_64-linux-clang") + ":" + args.host_library_path)

    def run(command, logfile):
        with (output / logfile).open("w") as log:
            subprocess.run([str(x) for x in command], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    # This is a checksum-verified official checkpoint; use the legacy ONNX exporter.
    export_code = """
import functools, os, sys
from pathlib import Path
os.environ['TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD']='1'
os.environ['YOLO_CONFIG_DIR']=str(Path(sys.argv[1])/'yolo-config')
import torch
torch.onnx.export=functools.partial(torch.onnx.export,dynamo=False)
from ultralytics import YOLO
YOLO(str(Path(sys.argv[1])/'yolov8n.pt')).export(format='onnx',imgsz=640,opset=17,simplify=False,dynamic=False)
"""
    run([args.export_python.absolute(), "-c", export_code, output], "export.log")
    with zipfile.ZipFile(output / "coco128.zip") as archive:
        for name in archive.namelist():
            if not (output / name).resolve().is_relative_to(output):
                raise SystemExit("Unsafe calibration archive member")
        archive.extractall(output)
    calibration_code = """
from pathlib import Path
import sys, cv2, numpy as np
p=Path(sys.argv[1]); raw=p/'calibration'; raw.mkdir(exist_ok=True)
entries=[]
for path in sorted((p/'coco128/images/train2017').glob('*.jpg'))[:32]+[p/'bus.jpg']:
    im=cv2.cvtColor(cv2.imread(str(path)),cv2.COLOR_BGR2RGB)
    h,w=im.shape[:2]; scale=min(640/w,640/h); rw,rh=round(w*scale),round(h*scale)
    out=np.full((640,640,3),114,np.uint8); left,top=(640-rw)//2,(640-rh)//2
    out[top:top+rh,left:left+rw]=cv2.resize(im,(rw,rh),interpolation=cv2.INTER_LINEAR)
    dest=raw/(path.stem+'.raw'); (out.astype(np.float32)/255).tofile(dest); entries.append(str(dest))
assert len(entries)==33
(p/'calibration.txt').write_text('\\n'.join(entries[:-1])+'\\n')
(p/'bus-input.txt').write_text(entries[-1]+'\\n')
"""
    run([args.export_python.absolute(), "-c", calibration_code, output], "calibration.log")
    tools = sdk / "bin/x86_64-linux-clang"
    run([args.converter_python.absolute(), tools / "qnn-onnx-converter", "--input_network", output / "yolov8n.onnx",
         "--output_path", output / "yolov8n.cpp", "--input_list", output / "calibration.txt",
         "--act_bitwidth", "16", "--weights_bitwidth", "8"], "convert.log")
    # The model wrapper exposes a C ABI and can use host GCC. No SDK source edits.
    generator = """
import runpy,sys
x=runpy.run_path(sys.argv[1])
for target in x['ModelLibGenerator'].available_targets:
    if target.alias=='x86_64-linux-clang': target._compiler='/usr/bin/g++'
sys.argv=sys.argv[1:]
x['main']()
"""
    run([args.converter_python.absolute(), "-c", generator, tools / "qnn-model-lib-generator",
         "-c", output / "yolov8n.cpp", "-b", output / "yolov8n.bin", "-o", output / "lib",
         "-t", "x86_64-linux-clang"], "model-lib.log")
    (output / "htp.json").write_text(json.dumps({"graphs": [{"graph_names": ["yolov8n"], "vtcm_mb": 2, "O": 3}],
                                                "devices": [{"soc_id": 35, "dsp_arch": "v68"}]}))
    (output / "context-config.json").write_text(json.dumps({"backend_extensions": {
        "shared_library_path": str(sdk / "lib/x86_64-linux-clang/libQnnHtpNetRunExtensions.so"),
        "config_file_path": str(output / "htp.json")}}))
    run([tools / "qnn-context-binary-generator", "--model", output / "lib/x86_64-linux-clang/libyolov8n.so",
         "--backend", sdk / "lib/x86_64-linux-clang/libQnnHtp.so", "--binary_file", "yolov8n",
         "--output_dir", output / "context", "--config_file", output / "context-config.json"], "context.log")
    run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-loop", "1", "-i", output / "bus.jpg",
         "-vf", "scale=810:1080,zoompan=z=1+0.001*on:x=iw/2-iw/zoom/2:y=ih/2-ih/zoom/2:d=150:s=810x1080:fps=15,format=yuv420p",
         "-t", "10", "-c:v", "libx264", "-profile:v", "baseline", "-pix_fmt", "yuv420p", "-y", output / "demo.mp4"], "demo-video.log")
    files = [output / name for name in ASSETS] + [output / "yolov8n.onnx", output / "context/yolov8n.bin", output / "demo.mp4"]
    manifest = {"schema_version": 1, "model": "YOLOv8n", "license": "AGPL-3.0", "sdk": sdk.name,
                "soc_id": 35, "dsp_arch": "v68", "weights_bits": 8, "activation_bits": 16,
                "calibration": "First 32 sorted COCO128 training images; bus.jpg excluded",
                "sources": {name: url for name, (url, _) in ASSETS.items()},
                "sha256": {str(p.relative_to(output)): hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    for name, executable in (("export", args.export_python), ("converter", args.converter_python)):
        manifest[name + "_packages"] = subprocess.check_output([str(executable.absolute()), "-m", "pip", "freeze"], text=True).splitlines()
    (output / "model-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(output / "context/yolov8n.bin")


if __name__ == "__main__":
    main()
