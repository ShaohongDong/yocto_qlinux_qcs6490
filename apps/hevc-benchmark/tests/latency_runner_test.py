# SPDX-License-Identifier: MIT
import importlib.machinery
import importlib.util
from pathlib import Path
import tempfile
import json
import signal
import subprocess
import sys
import time
import unittest
import struct
from unittest.mock import patch

path = Path(__file__).resolve().parents[1] / 'scripts/run-latency'
loader = importlib.machinery.SourceFileLoader('runner', str(path))
spec = importlib.util.spec_from_loader(loader.name, loader)
runner = importlib.util.module_from_spec(spec)
loader.exec_module(runner)


class GovernorsTests(unittest.TestCase):
    def test_restores_on_success_and_failure(self):
        for fail in (False, True):
            with tempfile.TemporaryDirectory() as d:
                root = Path(d)
                policy = root / 'policy0'
                policy.mkdir()
                governor = policy / 'scaling_governor'
                governor.write_text('schedutil\n')
                (policy / 'scaling_available_governors').write_text('schedutil performance\n')
                try:
                    with runner.Governors(root) as g:
                        g.enable_performance()
                        self.assertEqual(governor.read_text().strip(), 'performance')
                        if fail:
                            raise RuntimeError('Test child failure')
                except RuntimeError:
                    self.assertTrue(fail)
                self.assertEqual(governor.read_text().strip(), 'schedutil')

    def test_partial_setup_restores(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for i, available in enumerate(('performance schedutil', 'schedutil')):
                policy = root / f'policy{i}'
                policy.mkdir()
                (policy / 'scaling_governor').write_text('schedutil')
                (policy / 'scaling_available_governors').write_text(available)
            with self.assertRaises(RuntimeError):
                with runner.Governors(root) as g:
                    g.enable_performance()
            self.assertEqual((root / 'policy0/scaling_governor').read_text().strip(), 'schedutil')


class VoteTests(unittest.TestCase):
    def test_legal_vote_restored_on_exception(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / 'opp').mkdir()
            (root / 'opp/opp-hz').write_bytes(struct.pack('>Q', 335000000))
            vote = root / 'vote'
            vote.write_text('0\n')
            with self.assertRaisesRegex(RuntimeError, 'child failure'):
                with runner.VpuVote(335000000, vote, root):
                    self.assertEqual(int(vote.read_text()), 335000000)
                    raise RuntimeError('child failure')
            self.assertEqual(int(vote.read_text()), 0)
            with self.assertRaisesRegex(RuntimeError, 'OPP'):
                with runner.VpuVote(460000000, vote, root):
                    pass
            self.assertEqual(int(vote.read_text()), 0)

    def test_thermal_guard_requires_sensor_and_margin(self):
        for temperature in (None, 75000, 90000, -1):
            sample = {'thermal': {}}
            if temperature is not None:
                sample['thermal']['zone'] = dict(type='video-thermal', millidegrees_c=temperature)
            with self.assertRaises(RuntimeError):
                runner.require_safe_video_temperature(sample)
        runner.require_safe_video_temperature({'thermal': {'zone': dict(type='video-thermal', millidegrees_c=74000)}})


class CancellationTests(unittest.TestCase):
    def test_child_cancelled_and_report_saved(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            child = root / 'fake-benchmark'
            child.write_text('#!/bin/sh\nexec sleep 60\n')
            child.chmod(0o755)
            output = root / 'trial'
            process = subprocess.Popen([sys.executable, '-B', str(path), '--binary', str(child),
                                        '--output', str(output)])
            try:
                deadline = time.monotonic() + 5
                while not (output / 'console.log').exists() and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertTrue((output / 'console.log').exists())
                process.send_signal(signal.SIGTERM)
                self.assertEqual(process.wait(timeout=15), 1)
                result = json.loads((output / 'trial.json').read_text())
                self.assertEqual(result['status'], 'CANCELLED')
                self.assertIn('after', result)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()


class MatrixTests(unittest.TestCase):
    def test_ranking_and_quality_gate(self):
        matrix_loader = importlib.machinery.SourceFileLoader('matrix', str(path.with_name('run-latency-matrix')))
        spec = importlib.util.spec_from_loader(matrix_loader.name, matrix_loader)
        matrix = importlib.util.module_from_spec(spec)
        matrix_loader.exec_module(matrix)
        baseline = dict(profile='baseline', rate=0, performance=False, mean=34, p95=35)
        tiny = dict(profile='low', rate=120, performance=True, mean=33.8, p95=34.6)
        large = dict(profile='low', rate=120, performance=True, mean=20, p95=22)
        self.assertEqual(matrix.select([tiny, baseline])[0], baseline)
        self.assertEqual(matrix.select([large, baseline])[0], large)
        r = dict(status='PASS', eos=True, encode_fps=30, decode_fps=30,
                 y_psnr_from_pooled_mse_db=40)
        for key in ('missing_encoded_frames', 'missing_decoded_frames', 'missing_quality_frames',
                    'unmatched_timestamps', 'duplicate_timestamps', 'invalid_timing_frames'):
            r[key] = 0
        self.assertTrue(matrix.eligible(r, r))
        self.assertFalse(matrix.eligible(dict(r, y_psnr_from_pooled_mse_db=39), r))
        self.assertFalse(matrix.eligible(dict(r, missing_decoded_frames=1), r))
        self.assertFalse(matrix.eligible(dict(r, quality_mode='off'), r))


class StageTwoTests(unittest.TestCase):
    def load(self, name):
        loader = importlib.machinery.SourceFileLoader(name, str(path.with_name(name)))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        return module

    def test_quality_uses_same_frame_segment(self):
        stage = self.load('run-latency-stage2')
        def report(indices, error):
            return {'frames': [dict(source_index=i, y_squared_error=error, y_pixels=100) for i in indices]}
        a, b = report(range(150, 180),100), report(range(149,180),100)
        self.assertTrue(stage.paired_quality(a,b)['passed'])
        self.assertFalse(stage.paired_quality(a,report(range(150,180),200))['passed'])
        with self.assertRaises(ValueError):
            stage.paired_quality(a,report(range(180,210),100))

    def test_complete_matrix_handles_independent_codec_arguments(self):
        stage = self.load('run-latency-stage2')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            inputs = root/'inputs'; inputs.mkdir()
            clips = {name: dict(file=name+'.nv12', sha256=name) for name in ('checkers','texture')}
            (inputs/'manifest.json').write_text(json.dumps(dict(clips=clips)))
            commands = []
            def fake_run(cmd, check):
                commands.append(cmd)
                value = lambda flag: cmd[cmd.index(flag)+1]
                mode = value('--mode')
                if mode == 'encode' and value('--operating-rate') != '0':
                    raise subprocess.CalledProcessError(1,cmd)
                output = Path(value('--output'))/'reports/run'
                output.mkdir(parents=True)
                report = dict(status='PASS', eos=True, quality_validated=mode=='loopback',
                     quality_mode='full', encode_fps=30, decode_fps=30,
                     source_to_decode_ms=dict(mean=20 if value('--vpu-hz')!='0' else 30,
                                              p95=21 if value('--vpu-hz')!='0' else 31),
                     frames=[dict(source_index=i,y_squared_error=100,y_pixels=100) for i in range(3)])
                for key in ('missing_encoded_frames','missing_decoded_frames','missing_quality_frames',
                            'unmatched_timestamps','duplicate_timestamps','invalid_timing_frames'):
                    report[key]=0
                if '--input-nv12' in cmd:
                    report['input_sha256']=Path(value('--input-nv12')).stem
                (output/'report.json').write_text(json.dumps(report))
                return subprocess.CompletedProcess(cmd,0)
            argv = ['run-latency-stage2','--binary','fake','--inputs',str(inputs),
                    '--candidate-hz','460000048','--output',str(root/'matrix')]
            with patch.object(sys,'argv',argv), patch.object(stage.subprocess,'run',fake_run), patch('builtins.print'):
                self.assertEqual(stage.main(),0)
            result=json.loads((root/'matrix/summary.json').read_text())
            self.assertEqual(result['status'],'PASS')
            self.assertEqual(len(commands),21)
            self.assertEqual({cmd[cmd.index('--mode')+1] for cmd in commands},{'encode','decode','loopback'})

    def test_trace_maps_driver_timestamp_without_pts_equality(self):
        analyzer = self.load('analyze-latency-trace')
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root/'reports/run').mkdir(parents=True)
            (root/'reports/run/report.json').write_text(json.dumps({'frames': [dict(pts_ns=33333333,
                 encode_in_monotonic_ms=1000, encode_out_monotonic_ms=1020), dict(pts_ns=66666666,
                 encode_in_monotonic_ms=1010, encode_out_monotonic_ms=1030)]}))
            (root/'trace-stats.json').write_text(json.dumps({'cpu0':'overrun: 0\ndropped events: 0\n'}))
            (root/'trace.txt').write_text('task 1.001000: msm_v4l2_vidc_buffer_event_log: abcd: hevcE: qbuf: INPUT: idx 0 ts 99000000000 attr 0x1\n'
                                         'task 1.011000: msm_v4l2_vidc_buffer_event_log: abcd: hevcE: qbuf: INPUT: idx 1 ts 100000000000 attr 0x1\n'
                                         'irq 1.019000: msm_v4l2_vidc_buffer_event_log: abcd: hevcE: dqbuf: OUTPUT: idx 0 ts 99000000000 attr 0x10\n'
                                         'irq 1.029000: msm_v4l2_vidc_buffer_event_log: abcd: hevcE: dqbuf: OUTPUT: idx 1 ts 100000000000 attr 0x10\n')
            result = analyzer.analyze(root)
            self.assertEqual(result['status'],'PASS')
            self.assertEqual(result['codecs']['encode']['matched'],2)
            self.assertAlmostEqual(result['codecs']['encode']['segments_ms']['driver_firmware_ms']['mean'],18)
            (root/'trace-stats.json').write_text(json.dumps({'cpu0':'overrun: 1\n'}))
            with self.assertRaises(ValueError):
                analyzer.analyze(root)


if __name__ == '__main__':
    unittest.main()
