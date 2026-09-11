# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import sys
from pathlib import Path

spec = importlib.util.spec_from_file_location("generator", Path(__file__).with_name("generate-registers.py"))
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
source = Path(sys.argv[1]).read_bytes()
header = generator.generate(source)
assert "{0x0136, 0x1800, 2}" in header
assert "{0x0112, 0x000a, 1}" in header
try:
    generator.generate(source.replace(b"0x1800", b"0x1900", 1))
except ValueError:
    pass
else:
    raise AssertionError("Modified source was accepted")
print("PASS: pinned input required; 8-bit and 16-bit table entries preserved")
