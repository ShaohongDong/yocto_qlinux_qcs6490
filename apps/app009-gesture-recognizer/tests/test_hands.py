#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, unittest
from pathlib import Path
import numpy as np, torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from hand_frontend import (
    sample_rect,
    ANCHORS,
    decode_palms,
    project_landmarks,
    HandFrontend,
)
from cache_hands import sample_frames
from hand_model import HandTemporal, CHANNELS
from events import Decoder


class HandTests(unittest.TestCase):
    def test_twenty_hz_uses_real_source_frames(self):
        np.testing.assert_array_equal(sample_frames(9), [0, 2, 3, 5, 6, 8])
        for n in [1, 2, 3, 300]:
            ids = sample_frames(n)
            self.assertTrue((ids < n).all())
            self.assertEqual(len(ids), len(set(ids)))

    def test_affine_identity_and_padding(self):
        rng = np.random.default_rng(42)
        rgb = rng.integers(0, 256, (192, 192, 3), dtype=np.uint8)
        np.testing.assert_array_equal(sample_rect(rgb, [96, 96, 192, 0], 192), rgb)
        self.assertEqual(int(sample_rect(rgb, [-1000, -1000, 192, 0], 192).sum()), 0)

    def test_anchor_decode_empty_and_degenerate(self):
        self.assertEqual(ANCHORS.shape, (2016, 2))
        self.assertEqual(
            decode_palms(np.zeros((2016, 18)), np.full(2016, -100), 640, 480), []
        )
        self.assertEqual(
            decode_palms(np.zeros((2016, 18)), np.full(2016, 100), 640, 480), []
        )

    def test_projection_preserves_original_motion(self):
        raw = np.zeros((21, 3))
        raw[:, :2] = 112
        a = project_landmarks(raw, [100, 200, 50, 0])
        b = project_landmarks(raw, [110, 190, 50, 0])
        np.testing.assert_allclose(b - a, np.tile([10, -10, 0], (21, 1)))

    def test_v2_causality_and_absolute_information(self):
        torch.set_num_threads(2)
        torch.manual_seed(42)
        for branch, c in CHANNELS.items():
            m = HandTemporal(branch, 64).eval()
            x = torch.randn(1, c, 1, 64, requires_grad=True)
            y = m.sequence(x)
            changed = x.detach().clone()
            changed[:, :, :, 40:] += 50
            torch.testing.assert_close(
                y[:, :, :, :40], m.sequence(changed)[:, :, :, :40]
            )
            m(x).sum().backward()
            self.assertGreater(x.grad[:, :, :, :10].abs().sum().item(), 0)
            self.assertFalse(torch.allclose(m(x.detach()), m(x.detach() + 1)))

    def test_time_based_confirmation_and_release(self):
        d = Decoder(0.5, 0.2, sample_period=0.05, min_duration=0.1, stable_seconds=0.1)
        for t in [0, 2 / 30, 3 / 30, 5 / 30]:
            d.update(np.eye(12)[1], t)
        for t in [6 / 30, 8 / 30, 9 / 30, 11 / 30]:
            d.update(np.eye(12)[0], t)
        self.assertEqual(len(d.events), 1)
        self.assertAlmostEqual(d.events[0]["end"], 5 / 30 + 0.05)
        self.assertTrue(d.events[0]["complete"])

    def test_absent_hand_invalidates_without_fabricated_landmarks(self):
        def palm(_):
            return {
                "Identity": np.zeros((1, 2016, 18)),
                "Identity_1": np.full((1, 2016, 1), -100),
            }

        def hand(_):
            raise AssertionError("No palm must not invoke landmarks")

        f = HandFrontend(palm=palm, landmark=hand)
        r = f.step(np.zeros((32, 48, 3), np.uint8), 0)
        self.assertFalse(r["valid"])
        self.assertTrue(r["reset"])
        self.assertEqual(float(r["geometry"].sum()), 0)


if __name__ == "__main__":
    unittest.main()
