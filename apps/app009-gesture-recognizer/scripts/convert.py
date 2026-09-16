#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert one gesture graph to Q6A HTP W8A16; preserve range-correction audit."""

import argparse, json, os, subprocess, shutil, math
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--sdk", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--graph", choices=["features", "temporal"], required=True)
    p.add_argument("--converter-python", type=Path, required=True)
    p.add_argument("--host-library-path", default="")
    a = p.parse_args()
    sdk = a.sdk.resolve()
    root = a.output.resolve()
    g = a.graph
    o = root / g
    o.mkdir(exist_ok=True)
    bin = sdk / "bin/x86_64-linux-clang"
    env = dict(
        os.environ,
        PYTHONDONTWRITEBYTECODE="1",
        QNN_SDK_ROOT=str(sdk),
        PYTHONPATH=str(sdk / "lib/python"),
        LD_LIBRARY_PATH=str(sdk / "lib/x86_64-linux-clang") + ":" + a.host_library_path,
    )

    def run(cmd, log):
        with (o / log).open("w") as f:
            subprocess.run(
                list(map(str, cmd)),
                env=env,
                stdout=f,
                stderr=subprocess.STDOUT,
                check=True,
            )

    cmd = [
        a.converter_python.absolute(),
        bin / "qnn-onnx-converter",
        "--input_network",
        root / f"{g}.onnx",
        "--output_path",
        o / f"{g}.cpp",
        "--input_list",
        root / f"{g}-calibration.txt",
        "--use_per_channel_quantization",
        "--act_bitwidth",
        "16",
        "--weights_bitwidth",
        "8",
        "--bias_bitwidth",
        "32",
    ]
    run(cmd, "convert-initial.log")
    inspect = """
import json, re, sys, onnx
from onnx import numpy_helper
m=onnx.load(sys.argv[1])
names={re.sub(r'\\W','_',v):v for n in m.graph.node for v in n.output}
biases={re.sub(r'\\W','_',t.name):numpy_helper.to_array(t).tolist() for t in m.graph.initializer if len(t.dims)==1}
print(json.dumps({'names':names,'biases':biases}))
"""
    info = json.loads(
        subprocess.check_output(
            [
                str(a.converter_python.absolute()),
                "-c",
                inspect,
                str(root / f"{g}.onnx"),
            ],
            env=env,
            text=True,
        )
    )
    graph = json.loads((o / f"{g}_net.json").read_text())["graph"]
    overrides = {}
    audit = []
    for node in graph["nodes"].values():
        if node["type"] != "Conv2d" or len(node["input_names"]) != 3:
            continue
        input_name, _, bias_name = node["input_names"]
        if bias_name not in info["biases"] or input_name not in info["names"]:
            continue
        params = graph["tensors"][bias_name]["quant_params"]
        ranges = params.get("axis_scale_offset", {}).get("scale_offsets", [])
        if not ranges:
            ranges = [params.get("scale_offset", {})] * len(info["biases"][bias_name])
        if not all(e.get("maximum", 0) > 0 for e in ranges):
            continue
        ratio = max(
            abs(v) / e["maximum"] for v, e in zip(info["biases"][bias_name], ranges)
        )
        activation = graph["tensors"][input_name]["quant_params"].get(
            "scale_offset", {}
        )
        if (
            ratio <= 1
            or activation.get("minimum", -1) < 0
            or not 0 < activation.get("maximum", 0) <= 0.001
        ):
            continue
        maximum = 2 ** math.ceil(math.log2(activation["maximum"] * ratio * 2))
        name = info["names"][input_name]
        overrides[name] = [
            {
                "bitwidth": 16,
                "dtype": "int",
                "is_symmetric": "False",
                "min": 0.0,
                "max": maximum,
                "scale": maximum / 65535,
                "offset": 0,
            }
        ]
        audit.append(
            {
                "tensor": name,
                "bias_tensor": bias_name,
                "original_maximum": activation["maximum"],
                "bias_range_overflow_ratio": ratio,
                "corrected_maximum": maximum,
            }
        )
    (o / "quantization-audit.json").write_text(json.dumps(audit, indent=2) + "\n")
    if overrides:
        shutil.copy2(o / f"{g}_net.json", o / "initial-quantized-net.json")
        (o / "overrides.json").write_text(
            json.dumps(
                {
                    "version": "0.6.1",
                    "activation_encodings": overrides,
                    "param_encodings": {},
                },
                indent=2,
            )
            + "\n"
        )
        run(cmd + ["--quantization_overrides", o / "overrides.json"], "convert.log")
    else:
        shutil.copy2(o / "convert-initial.log", o / "convert.log")
    generator = 'import runpy,sys\nx=runpy.run_path(sys.argv[1])\nfor target in x["ModelLibGenerator"].available_targets:\n if target.alias=="x86_64-linux-clang": target._compiler="/usr/bin/g++"\nsys.argv=sys.argv[1:]\nx["main"]()'
    run(
        [
            a.converter_python.absolute(),
            "-c",
            generator,
            bin / "qnn-model-lib-generator",
            "-c",
            o / f"{g}.cpp",
            "-b",
            o / f"{g}.bin",
            "-o",
            o / "lib",
            "-t",
            "x86_64-linux-clang",
        ],
        "model-lib.log",
    )
    (o / "htp.json").write_text(
        json.dumps(
            {
                "graphs": [{"graph_names": [g], "vtcm_mb": 2, "O": 3}],
                "devices": [{"soc_id": 35, "dsp_arch": "v68"}],
            }
        )
    )
    (o / "context-config.json").write_text(
        json.dumps(
            {
                "backend_extensions": {
                    "shared_library_path": str(
                        sdk / "lib/x86_64-linux-clang/libQnnHtpNetRunExtensions.so"
                    ),
                    "config_file_path": str(o / "htp.json"),
                }
            }
        )
    )
    run(
        [
            bin / "qnn-context-binary-generator",
            "--model",
            o / f"lib/x86_64-linux-clang/lib{g}.so",
            "--backend",
            sdk / "lib/x86_64-linux-clang/libQnnHtp.so",
            "--binary_file",
            g,
            "--output_dir",
            o / "context",
            "--config_file",
            o / "context-config.json",
        ],
        "context.log",
    )
    print(o / "context" / f"{g}.bin", flush=True)


if __name__ == "__main__":
    main()
