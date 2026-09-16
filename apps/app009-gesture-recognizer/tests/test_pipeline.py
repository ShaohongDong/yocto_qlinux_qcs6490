#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import sys, unittest
from pathlib import Path
import numpy as np
import torch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from events import Decoder, event_metrics
from model import Temporal, Features, ARCHITECTURES, architecture_name


class PipelineTests(unittest.TestCase):
    def test_temporal_causality_and_gradient(self):
        torch.manual_seed(42)
        torch.set_num_threads(2)
        model = Temporal()
        x = torch.randn(1, 576, 1, 48, requires_grad=True)
        y = model.sequence(x)
        other = x.detach().clone()
        other[:, :, :, 32:] += 100
        np.testing.assert_allclose(
            y.detach().numpy()[:, :, :, :32],
            model.sequence(other).detach().numpy()[:, :, :, :32],
            atol=1e-6,
            rtol=0,
        )
        y[:, :, :, -1].sum().backward()
        self.assertGreater(float(x.grad[:, :, :, -2].abs().sum()), 0)

    def test_48_window_uses_history_beyond_32_samples(self):
        torch.manual_seed(42)
        torch.set_num_threads(2)
        m = Temporal(48)
        x = torch.randn(1, 576, 1, 48, requires_grad=True)
        m(x).sum().backward()
        self.assertGreater(float(x.grad[:, :, :, :16].abs().sum()), 0)

    def test_architecture_contracts(self):
        torch.set_num_threads(2)
        with self.assertRaises(ValueError):
            architecture_name("unknown")
        with torch.inference_mode():
            x = torch.randn(1, 3, 224, 224)
            for architecture in ARCHITECTURES:
                model = Features(False, architecture).eval()
                middle = model.prefix(x)
                self.assertEqual(tuple(middle.shape[1:]), model.prefix_shape)
                result = model.tail(middle)
                self.assertEqual(tuple(result.shape), (1, 576))
                torch.testing.assert_close(result, model(x))

    def test_relative_temporal_is_causal_and_offset_invariant(self):
        torch.set_num_threads(2)
        model = Temporal(48, "regularized").eval()
        x = torch.randn(2, 576, 1, 48)
        offset = torch.randn(2, 576, 1, 1)
        torch.testing.assert_close(model(x), model(x + offset), atol=1e-5, rtol=1e-5)
        changed = x.clone()
        changed[:, :, :, 32:] += 10
        torch.testing.assert_close(
            model.sequence(x)[:, :, :, :32], model.sequence(changed)[:, :, :, :32]
        )

    def test_duplicate_events_count_as_false_positive(self):
        t = {"v": [{"label": 1, "start": 0, "end": 1}]}
        e = {
            "label": 1,
            "start": 0,
            "end": 1,
            "confirmed_at": 1.5,
            "confidence": 0.9,
            "complete": True,
        }
        m = event_metrics({"v": [e, e]}, t, 60)
        self.assertEqual(m["tp"][1], 1)
        self.assertEqual(m["fp"][1], 1)

    def test_wrong_class_and_background_are_not_matches(self):
        t = {"v": [{"label": 1, "start": 0, "end": 1}]}
        e = {
            "label": 2,
            "start": 2,
            "end": 3,
            "confirmed_at": 3.5,
            "confidence": 0.9,
            "complete": True,
        }
        m = event_metrics({"v": [e]}, t, 60)
        self.assertEqual(m["fn"][1], 1)
        self.assertEqual(m["background_false_events_per_minute"], 1)

    def test_incomplete_tail_is_not_confirmed_event(self):
        d = Decoder()
        for i in range(10):
            d.update(np.eye(12)[2], i / 10)
        d.close(1, False)
        self.assertEqual(len(d.events), 1)
        self.assertFalse(d.events[0]["complete"])


if __name__ == "__main__":
    unittest.main()
