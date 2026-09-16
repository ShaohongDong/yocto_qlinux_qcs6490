# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("prepare", Path(__file__).resolve().parents[1] / "scripts/prepare-data.py")
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)


class DataTests(unittest.TestCase):
    def test_split_eligibility_and_reproducibility(self):
        rows = [{"image_id": i, "num_keypoints": 1} for i in range(50)]
        rows += [{"image_id": 900, "num_keypoints": 0},
                 {"image_id": 901, "num_keypoints": 17, "iscrowd": 1}]
        ids = prepare.choose(rows, 40, 42)
        self.assertEqual(ids, prepare.choose(list(reversed(rows)), 40, 42))
        self.assertEqual(len(set(ids)), 40)
        self.assertTrue(set(ids[:30]).isdisjoint(ids[30:]))
        self.assertNotIn(900, ids)
        self.assertNotIn(901, ids)

    def test_pose_label_coordinates_and_visibility(self):
        row = {"bbox": [20, 10, 40, 20], "keypoints": [40, 20, 2] * 17}
        values = [float(v) for v in prepare.label(row, 100, 50).split()]
        self.assertEqual(len(values), 56)
        self.assertEqual(values[:5], [0, .4, .4, .4, .4])
        self.assertEqual(values[5:8], [.4, .4, 2])

    def test_zero_keypoint_person_remains_positive_box(self):
        row = {"bbox": [20, 10, 40, 20], "keypoints": [0, 0, 0] * 17,
               "num_keypoints": 0, "iscrowd": 0}
        self.assertTrue(prepare.trainable(row))
        values = [float(v) for v in prepare.label(row, 100, 50).split()]
        self.assertEqual(values[:5], [0, .4, .4, .4, .4])
        self.assertEqual(values[5:], [0] * 51)
        self.assertFalse(prepare.trainable({**row, "iscrowd": 1}))
        self.assertFalse(prepare.trainable({**row, "bbox": [20, 10, 0, 20]}))


if __name__ == "__main__":
    unittest.main()
