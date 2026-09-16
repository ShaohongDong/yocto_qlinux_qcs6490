#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert the trained ONNX to Q6A QNN HTP context using QAIRT 2.47."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='Training output containing pose.onnx and calibration.txt')
    parser.add_argument('--converter-python', type=Path, required=True)
    parser.add_argument('--host-library-path', default='')
    args = parser.parse_args()
    sdk, output = args.sdk.resolve(), args.output.resolve()
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1", QNN_SDK_ROOT=str(sdk),
               PYTHONPATH=str(sdk / "lib/python"),
               LD_LIBRARY_PATH=str(sdk / "lib/x86_64-linux-clang") + ":" + args.host_library_path)

    def run(command, logfile):
        with (output / logfile).open("w") as log:
            subprocess.run([str(x) for x in command], env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    tools = sdk / "bin/x86_64-linux-clang"
    command = [args.converter_python.absolute(), tools / "qnn-onnx-converter", "--input_network", output / "pose.onnx",
               "--output_path", output / "pose.cpp", "--input_list", output / "calibration.txt",
               "--use_per_channel_quantization", "--bias_bitwidth", "32", "--act_bitwidth", "16", "--weights_bitwidth", "8"]
    run(command, "convert-initial.log")
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
         "-c", output / "pose.cpp", "-b", output / "pose.bin", "-o", output / "lib",
         "-t", "x86_64-linux-clang"], "model-lib.log")
    (output / "htp.json").write_text(json.dumps({"graphs": [{"graph_names": ["pose"], "vtcm_mb": 2, "O": 3}],
                                                "devices": [{"soc_id": 35, "dsp_arch": "v68"}]}))
    (output / "context-config.json").write_text(json.dumps({"backend_extensions": {
        "shared_library_path": str(sdk / "lib/x86_64-linux-clang/libQnnHtpNetRunExtensions.so"),
        "config_file_path": str(output / "htp.json")}}))
    run([tools / "qnn-context-binary-generator", "--model", output / "lib/x86_64-linux-clang/libpose.so",
         "--backend", sdk / "lib/x86_64-linux-clang/libQnnHtp.so", "--binary_file", "pose",
         "--output_dir", output / "context", "--config_file", output / "context-config.json"], "context.log")
    training = json.loads((output/'training-manifest.json').read_text())
    files = ['pose.onnx', 'context/pose.bin', 'training-manifest.json']
    manifest = {'schema_version': 1, 'model': 'YOLOv8n-pose', 'sdk': sdk.name,
                'trained': training['trained'],
                'input': training['input'],
                'soc_id': 35, 'dsp_arch': 'v68', 'weights_bits': 8,
                'activation_bits': 16, 'per_channel_weights': True, 'bias_bits': 32,
                'calibration': training['calibration'],
                'sha256': {name: hashlib.sha256((output/name).read_bytes()).hexdigest() for name in files}}
    (output/'model-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(output/'context/pose.bin')


if __name__ == '__main__':
    main()
