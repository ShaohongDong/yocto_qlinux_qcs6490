# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

try:
    from PIL import Image
    SPEC = importlib.util.spec_from_file_location(
        "check_photos", Path(__file__).resolve().parents[1] / "scripts/check-native-photos.py")
    CHECK = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(CHECK)
except ImportError:
    CHECK = None


@unittest.skipIf(CHECK is None, "numpy and Pillow required")
class NativePhotoTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.jpeg = Path(self.directory.name) / "frame.jpg"
        Image.new("RGB", (1152, 648), (0, 0, 0)).save(self.jpeg, quality=95)
        # Constant Bayer black level 64, packed in MIPI RAW10 groups.
        self.jpeg.with_suffix(".raw").write_bytes(bytes([16, 16, 16, 16, 0]) * (2304 * 1296 // 4))
        self.metadata = {"capture_complete": True, "backend": "native", "format": "pRAA",
                         "width": 2304, "height": 1296, "stride": 2880, "bytes": 3732480,
                         "jpeg_width": 1152, "jpeg_height": 648, "black_level": 64,
                         "red_gain": 1, "blue_gain": 1, "gamma": 2.2,
                         "sequence": 1, "timestamp_monotonic_ns": 1234567}
        self.write_metadata()

    def write_metadata(self):
        self.jpeg.with_suffix(".json").write_text(json.dumps(self.metadata))

    def test_matching_photo_group(self):
        self.assertEqual(CHECK.validate(self.jpeg)["rgb_mae"], 0)

    def test_incomplete_photo_is_rejected(self):
        self.metadata["capture_complete"] = False
        self.write_metadata()
        with self.assertRaises(ValueError):
            CHECK.validate(self.jpeg)

    def test_truncated_raw_is_rejected(self):
        self.jpeg.with_suffix(".raw").write_bytes(b"short")
        with self.assertRaises(ValueError):
            CHECK.validate(self.jpeg)

    def test_jpeg_from_another_frame_is_rejected(self):
        Image.new("RGB", (1152, 648), (128, 128, 128)).save(self.jpeg, quality=95)
        with self.assertRaises(ValueError):
            CHECK.validate(self.jpeg)


if __name__ == "__main__":
    unittest.main()
