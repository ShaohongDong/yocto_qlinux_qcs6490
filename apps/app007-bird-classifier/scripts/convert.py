#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert the trained ONNX to Q6A QNN HTP context using QAIRT 2.47."""
import argparse
import hashlib
import json
import math
import shutil
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='Training output containing bird.onnx and calibration.txt')
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
    command = [args.converter_python.absolute(), tools / "qnn-onnx-converter", "--input_network", output / "bird.onnx",
               "--output_path", output / "bird.cpp", "--input_list", output / "calibration.txt",
               "--use_per_channel_quantization", "--bias_bitwidth", "32", "--act_bitwidth", "16", "--weights_bitwidth", "8"]
    run(command, "convert-initial.log")
    # A near-zero SE activation scale can make int32 bias ranges too small.
    # Derive a safe range from trained biases and quantized representable ranges,
    # using only calibration statistics and model parameters (never test labels).
    inspect = """
import json, re, sys, onnx
from onnx import numpy_helper
m=onnx.load(sys.argv[1])
names={re.sub(r'\\W','_',v):v for n in m.graph.node for v in n.output}
biases={re.sub(r'\\W','_',t.name):numpy_helper.to_array(t).tolist() for t in m.graph.initializer if len(t.dims)==1}
print(json.dumps({'names':names,'biases':biases}))
"""
    info = json.loads(subprocess.check_output([str(args.converter_python.absolute()), '-c', inspect, str(output/'bird.onnx')], env=env, text=True))
    graph = json.loads((output/'bird_net.json').read_text())['graph']
    overrides = {}; audit = []
    for node in graph['nodes'].values():
        if node['type'] != 'Conv2d' or len(node['input_names']) != 3:
            continue
        input_name, _, bias_name = node['input_names']
        if bias_name not in info['biases'] or input_name not in info['names']:
            continue
        params = graph['tensors'][bias_name]['quant_params']
        ranges = params.get('axis_scale_offset', {}).get('scale_offsets', [])
        if not ranges:
            ranges = [params.get('scale_offset', {})] * len(info['biases'][bias_name])
        if not all(e.get('maximum', 0) > 0 for e in ranges):
            continue
        ratio = max(abs(v)/e['maximum'] for v,e in zip(info['biases'][bias_name], ranges))
        activation = graph['tensors'][input_name]['quant_params'].get('scale_offset', {})
        if ratio <= 1 or activation.get('minimum', -1) < 0 or not 0 < activation.get('maximum', 0) <= .001:
            continue
        maximum = 2 ** math.ceil(math.log2(activation['maximum'] * ratio * 2))
        name = info['names'][input_name]
        overrides[name] = [{'bitwidth':16,'dtype':'int','is_symmetric':'False','min':0.,'max':maximum,'scale':maximum/65535,'offset':0}]
        audit.append({'tensor':name,'bias_tensor':bias_name,'original_maximum':activation['maximum'],
                      'bias_range_overflow_ratio':ratio,'corrected_maximum':maximum})
    (output/'quantization-audit.json').write_text(json.dumps(audit,indent=2)+'\n')
    if overrides:
        shutil.copy2(output/'bird_net.json', output/'initial-quantized-net.json')
        (output/'overrides.json').write_text(json.dumps({'version':'0.6.1','activation_encodings':overrides,'param_encodings':{}},indent=2)+'\n')
        run(command + ['--quantization_overrides', output/'overrides.json'], 'convert.log')
    else:
        shutil.copy2(output/'convert-initial.log', output/'convert.log')
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
         "-c", output / "bird.cpp", "-b", output / "bird.bin", "-o", output / "lib",
         "-t", "x86_64-linux-clang"], "model-lib.log")
    (output / "htp.json").write_text(json.dumps({"graphs": [{"graph_names": ["bird"], "vtcm_mb": 2, "O": 3}],
                                                "devices": [{"soc_id": 35, "dsp_arch": "v68"}]}))
    (output / "context-config.json").write_text(json.dumps({"backend_extensions": {
        "shared_library_path": str(sdk / "lib/x86_64-linux-clang/libQnnHtpNetRunExtensions.so"),
        "config_file_path": str(output / "htp.json")}}))
    run([tools / "qnn-context-binary-generator", "--model", output / "lib/x86_64-linux-clang/libbird.so",
         "--backend", sdk / "lib/x86_64-linux-clang/libQnnHtp.so", "--binary_file", "bird",
         "--output_dir", output / "context", "--config_file", output / "context-config.json"], "context.log")
    (output / 'LICENSE-model').write_text('Model architecture and pretrained weights: torchvision BSD-3-Clause; see LICENSE-torchvision. Fine-tuned on CUB-200-2011, non-commercial research/education only. CUB may overlap ImageNet pretraining. Dataset: https://www.vision.caltech.edu/datasets/cub_200_2011/\n')
    files = ['bird.onnx', 'context/bird.bin', 'labels.txt', 'training-manifest.json', 'quantization-audit.json']
    manifest = {'schema_version': 1, 'model': 'MobileNetV3-Small CUB20', 'sdk': sdk.name,
                'soc_id': 35, 'dsp_arch': 'v68', 'weights_bits': 8, 'activation_bits': 16, 'per_channel_weights': True, 'bias_bits': 32, 'range_corrections': audit,
                'calibration': '10 training images per class; validation and test excluded',
                'sha256': {name: hashlib.sha256((output/name).read_bytes()).hexdigest() for name in files}}
    (output/'model-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(output/'context/bird.bin')


if __name__ == '__main__':
    main()
