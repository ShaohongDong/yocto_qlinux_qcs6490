#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fetch pinned external reference interfaces/tools into a development directory."""
import argparse
import hashlib
import json
import urllib.request
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads((Path(__file__).resolve().parents[1] / "docs/chi-reference-inputs.json").read_text())
    root = args.output.resolve()
    for name, expected in manifest["files"].items():
        path = root / name
        if path.exists():
            data = path.read_bytes()
        else:
            url = f'https://raw.githubusercontent.com/{manifest["repo"]}/{manifest["commit"]}/chi-cdk/{name}'
            with urllib.request.urlopen(url, timeout=60) as response:
                data = response.read()
        if hashlib.sha256(data).hexdigest() != expected:
            raise SystemExit(f"SHA-256 mismatch: {name}; existing files were not overwritten")
        path.parent.mkdir(parents=True, exist_ok=True)
        if not path.exists():
            path.write_bytes(data)
        if name.endswith("ParameterParserGCC7"):
            path.chmod(path.stat().st_mode | 0o100)
    print(f'Fetched/verified {len(manifest["files"])} reference inputs at {root}')
    print("Reference version only; not a declaration of current Kodiak ABI compatibility.")


if __name__ == "__main__":
    main()
