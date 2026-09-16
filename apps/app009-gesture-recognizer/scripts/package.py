#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build a checksummed app-only release; require trained and accepted models."""

import argparse, json, hashlib, shutil, subprocess, tarfile, re, configparser
from pathlib import Path


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--build", type=Path, required=True)
    p.add_argument("--model", type=Path, required=True)
    p.add_argument("--assets", type=Path, required=True)
    p.add_argument("--dsp", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--version", required=True)
    p.add_argument(
        "--development",
        action="store_true",
        help="Explicit unaccepted development bundle; manifest preserves failed targets",
    )
    a = p.parse_args()
    model = json.loads((a.model / "model-manifest.json").read_text())
    training = json.loads((a.model / "training-manifest.json").read_text())
    selection = json.loads((a.model / "selection.json").read_text())
    if not model.get("trained") or not training.get("trained"):
        raise RuntimeError("Refusing untrained models")
    if not a.development and not model.get("accuracy_pass"):
        raise RuntimeError(
            "Model accuracy not accepted; use explicit development bundle only"
        )
    config = configparser.ConfigParser()
    config.read(a.model / "model.ini")
    if config.getboolean("gesture", "accuracy_accepted", fallback=False) != bool(
        model.get("accuracy_pass")
    ):
        raise RuntimeError("Model quality label does not match the accuracy result")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", a.version):
        raise ValueError("Invalid version")
    if a.output.exists():
        raise RuntimeError("Output already exists")
    for name, expected in model["sha256"].items():
        path = a.model / name
        if (
            not path.is_file()
            or hashlib.sha256(path.read_bytes()).hexdigest() != expected
        ):
            raise RuntimeError("Model hash mismatch: " + name)
    subprocess.run(
        ["cmake", "--install", str(a.build), "--prefix", str(a.output.resolve())],
        check=True,
    )
    root = a.output
    source = Path(__file__).resolve().parent
    for name in ["run", "install", "uninstall"]:
        shutil.copy2(source / name, root / name)
        (root / name).chmod(0o755)
    dest = root / "share/gesture-recognizer/models"
    dest.mkdir(parents=True)
    for name in [
        "features.bin",
        "temporal.bin",
        "model.ini",
        "model-manifest.json",
        "training-manifest.json",
        "selection.json",
        "labels.txt",
        "LICENSE-model",
        "LICENSE-torchvision",
    ]:
        shutil.copy2(a.model / name, dest / name)
    data = root / "share/gesture-recognizer/data"
    data.mkdir()
    for name in ["sample.mp4", "sources.json"]:
        shutil.copy2(a.assets / name, data / name)
    (root / "dsp").mkdir()
    for name in [
        "fastrpc_shell_unsigned_3",
        "libc++.so.1",
        "libc++abi.so.1",
        "LICENSE.qcom",
    ]:
        shutil.copy2(a.dsp / name, root / "dsp" / name)
    (root / "VERSION").write_text(a.version + "\n")
    (root / "RELEASE.json").write_text(
        json.dumps(
            {
                "version": a.version,
                "development": a.development,
                "accuracy_pass": model.get("accuracy_pass", False),
                "window": selection["window"],
            },
            indent=2,
        )
        + "\n"
    )
    hashes = [
        hashlib.sha256(f.read_bytes()).hexdigest() + "  " + str(f.relative_to(root))
        for f in sorted(root.rglob("*"))
        if f.is_file()
    ]
    (root / "SHA256SUMS").write_text("\n".join(hashes) + "\n")
    with tarfile.open(str(root) + ".tar.gz", "w:gz") as tar:
        tar.add(root, arcname=root.name)
    print(str(root) + ".tar.gz")


if __name__ == "__main__":
    main()
