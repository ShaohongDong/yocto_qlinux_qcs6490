#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.machinery
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


def load(name):
    loader = importlib.machinery.SourceFileLoader(name.replace('-', '_'), str(Path(__file__).parents[1] / 'scripts' / name))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class SuiteTests(unittest.TestCase):
    def test_fixed_windows_include_empty_buckets(self):
        suite = load('run-suite')
        report = dict(bandwidth_valid=True, bandwidth_100ms_bytes=[1000] + [0] * 9 + [2000] + [0] * 9)
        result = suite.bandwidth(report)
        self.assertEqual(result['100ms']['min_bps'], 0)
        self.assertEqual(result['100ms']['max_bps'], 160000)
        self.assertEqual(result['1000ms']['mean_bps'], 12000)
        self.assertEqual(result['1000ms']['max_bps'], 16000)
        report['bandwidth_valid'] = False
        self.assertIsNone(suite.bandwidth(report))

    def test_index_reordered_display_frames(self):
        module = load('analyze-quality')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.h265'
            path.write_bytes(b'abcdefgh')
            info = dict(frames=[dict(pkt_pos=0, pkt_size=2, key_frame=1),
                                dict(pkt_pos=4, pkt_size=2), dict(pkt_pos=6, pkt_size=2),
                                dict(pkt_pos=2, pkt_size=2)])
            module.probe = lambda path: info
            module.index_stream(path)
            self.assertEqual(Path(str(path) + '.index').read_text(), '0 2 0\n2 2 3\n4 2 1\n6 2 2\n')
            info['frames'][1]['pkt_pos'] = 3
            with self.assertRaises(ValueError):
                module.index_stream(path)

    def test_ssim_log_parenthesis_and_infinity(self):
        module = load('analyze-quality')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'ssim.log'
            path.write_text('n:1 Y:1.000000 U:1.000000 V:1.000000 All:1.000000 (inf)\n')
            self.assertEqual(module.log_rows(path)[0]['Y'], 1.0)

    def test_atomic_manifest_replacement(self):
        module = load('run-suite')
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'state.json'
            module.save(path, {'status': 'RUNNING'})
            module.save(path, {'status': 'PASS'})
            self.assertEqual(json.loads(path.read_text())['status'], 'PASS')
            self.assertFalse(path.with_suffix('.json.tmp').exists())


if __name__ == '__main__':
    unittest.main()
