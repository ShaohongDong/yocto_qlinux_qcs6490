#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Package a CMake install tree and prepared model as a relocatable board bundle."""
import argparse
import hashlib
import json
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
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--selection", required=True, type=Path, help="Frozen select-model.py selection.json")
    parser.add_argument("--dsp", required=True, type=Path,
                        help="Q6A CDSP directory from hexagon-dsp-binaries sysroot")
    args = parser.parse_args()
    manifest = json.loads((args.model / "model-manifest.json").read_text())
    selection = json.loads(args.selection.read_text())
    training = json.loads((args.model / "training-manifest.json").read_text())
    if selection["checkpoint_sha256"] != training["checkpoint_sha256"] or selection["trained"] != manifest["trained"]:
        raise SystemExit("Selected checkpoint does not match packaged model")
    for name, expected in manifest["sha256"].items():
        if hashlib.sha256((args.model / name).read_bytes()).hexdigest() != expected:
            raise SystemExit("Model artifact checksum mismatch: " + name)
    root = args.output.resolve()
    if root.exists():
        raise SystemExit("Output must not already exist")
    subprocess.run(["cmake", "--install", str(args.build), "--prefix", str(root)], check=True)
    shutil.copy2(args.selection, root / "selection.json")
    source = Path(__file__).resolve().parent
    for name in ("run", "install", "uninstall"):
        shutil.copy2(source / name, root / name)
        (root / name).chmod(0o755)
    data = root / "share/person-follow"
    (data / "models").mkdir(parents=True)
    (data / "data").mkdir()
    shutil.copy2(args.model / "context/pose.bin", data / "models/pose.bin")
    shutil.copy2(args.model / "model-manifest.json", data / "models/model-manifest.json")
    shutil.copy2(args.model / "training-manifest.json", data / "models/training-manifest.json")
    shutil.copy2(args.model / "LICENSE-model", data / "models/LICENSE-model")
    for name in ("sample.png", "sample.mp4", "sources.json"):
        shutil.copy2(args.assets / name, data / "data" / name)
    (root / "dsp").mkdir()
    for name in ("fastrpc_shell_unsigned_3", "libc++.so.1", "libc++abi.so.1", "LICENSE.qcom"):
        shutil.copy2(args.dsp / name, root / "dsp" / name)
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
