#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package a CMake install tree and prepared model as a relocatable board bundle."""
import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import tarfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--dsp", required=True, type=Path,
                        help="Q6A CDSP directory from hexagon-dsp-binaries sysroot")
    args = parser.parse_args()
    root = args.output.resolve()
    if root.exists():
        raise SystemExit("Output must not already exist")
    subprocess.run(["cmake", "--install", str(args.build), "--prefix", str(root)], check=True)
    source = Path(__file__).resolve().parent
    for name in ("run", "install", "uninstall"):
        shutil.copy2(source / name, root / name)
        (root / name).chmod(0o755)
    data = root / "share/ai-demo"
    (data / "models").mkdir(parents=True)
    (data / "data").mkdir()
    shutil.copy2(args.model / "context/yolov8n.bin", data / "models/yolov8n.bin")
    shutil.copy2(args.model / "model-manifest.json", data / "models/model-manifest.json")
    shutil.copy2(args.model / "LICENSE-model", data / "models/LICENSE-model")
    shutil.copy2(args.model / "bus.jpg", data / "data/bus.jpg")
    (root / "dsp").mkdir()
    for name in ("fastrpc_shell_unsigned_3", "libc++.so.1", "libc++abi.so.1", "LICENSE.qcom"):
        shutil.copy2(args.dsp / name, root / "dsp" / name)
    if (args.model / "demo.mp4").exists():
        shutil.copy2(args.model / "demo.mp4", data / "data/demo.mp4")
    (root / "VERSION").write_text(args.version + "\n")
    hashes = []
    for path in sorted(root.rglob("*")):
        if path.is_file():
            hashes.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + str(path.relative_to(root)))
    (root / "SHA256SUMS").write_text("\n".join(hashes) + "\n")
    with tarfile.open(str(root) + ".tar.gz", "w:gz") as archive:
        archive.add(root, arcname=root.name)
    print(str(root) + ".tar.gz")


if __name__ == "__main__":
    main()
