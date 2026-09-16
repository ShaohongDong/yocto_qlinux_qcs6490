#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail unless the actual GPU executes forward/backward and optimizer steps."""
import argparse
import json
from pathlib import Path
import subprocess
import time
import torch


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    result = {"status": "FAIL", "torch": torch.__version__, "cuda_runtime": torch.version.cuda}
    try:
        smi = subprocess.run(["nvidia-smi"], text=True, capture_output=True, timeout=30)
        result["nvidia_smi"] = smi.stdout + smi.stderr
        if smi.returncode:
            raise RuntimeError(f"nvidia-smi failed ({smi.returncode}); check driver activation after reboot")
        if not torch.cuda.is_available():
            raise RuntimeError("CUDA is not available; CPU training is not authorized as fallback")
        result["gpu"] = torch.cuda.get_device_name(0)
        result["capability"] = torch.cuda.get_device_capability(0)
        result["compiled_architectures"] = torch.cuda.get_arch_list()
        model = torch.nn.Sequential(torch.nn.Conv2d(3, 16, 3), torch.nn.ReLU(),
                                    torch.nn.AdaptiveAvgPool2d(1), torch.nn.Flatten(),
                                    torch.nn.Linear(16, 2)).cuda()
        optimizer = torch.optim.AdamW(model.parameters(), lr=.001)
        image = torch.randn(4, 3, 416, 416, device="cuda")
        truth = torch.tensor([0, 1, 0, 1], device="cuda")
        before = next(model.parameters()).detach().clone()
        start = time.monotonic()
        for _ in range(10):
            optimizer.zero_grad()
            loss = torch.nn.functional.cross_entropy(model(image), truth)
            if not torch.isfinite(loss):
                raise RuntimeError("Non-finite CUDA loss")
            loss.backward()
            optimizer.step()
        torch.cuda.synchronize()
        if torch.equal(before, next(model.parameters())):
            raise RuntimeError("CUDA optimizer did not update weights")
        result.update(status="PASS", seconds=time.monotonic() - start,
                      loss=float(loss.detach()), peak_bytes=torch.cuda.max_memory_allocated())
    except Exception as error:
        result["error"] = str(error)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["status"] == "PASS" else 1)


if __name__ == "__main__":
    main()
