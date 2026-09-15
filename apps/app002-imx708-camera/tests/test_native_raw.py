# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import unittest


def load(name, filename):
    spec = importlib.util.spec_from_file_location(
        name, Path(__file__).resolve().parents[1] / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CAPTURE = load("capture", "capture-native-raw.py")


class CaptureValidationTests(unittest.TestCase):
    def line(self, seq, used=3732480, timestamp=None, flags="ts-monotonic"):
        return (f"cap dqbuf: 0 seq: {seq} bytesused: {used} "
                f"ts: {timestamp if timestamp is not None else seq + 1}.0 ({flags})")

    def test_complete_buffers(self):
        records = CAPTURE.validate_buffers("\n".join(self.line(i) for i in range(3)), 3, 3732480)
        self.assertEqual([r["sequence"] for r in records], [0, 1, 2])

    def test_rejects_loss_short_buffer_error_and_bad_timestamp(self):
        cases = [self.line(0) + "\n" + self.line(2),
                 self.line(0) + "\n" + self.line(1, used=0),
                 self.line(0) + "\n" + self.line(1, flags="ts-monotonic, error"),
                 self.line(0) + "\n" + self.line(1, timestamp=1), self.line(0)]
        for output in cases:
            with self.subTest(output=output), self.assertRaises(RuntimeError):
                CAPTURE.validate_buffers(output, 2, 3732480)

    def test_raw10_unpack_low_bits(self):
        try:
            preview = load("preview", "preview-native-raw.py")
        except ImportError:
            self.skipTest("numpy and Pillow required")
        # Four known pixels 0, 341, 682, 1023; two padding bytes excluded.
        pixels = preview.unpack(bytes([0, 85, 170, 255, 228, 99, 99]), 4, 1, 7)
        self.assertEqual(pixels.tolist(), [[0, 341, 682, 1023]])


if __name__ == "__main__":
    unittest.main()
