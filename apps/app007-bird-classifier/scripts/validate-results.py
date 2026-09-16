#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare independent test results; execution PASS alone is not accuracy PASS."""
import argparse
import json
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--float-metrics', type=Path, required=True)
p.add_argument('--board-report', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
f = json.loads(a.float_metrics.read_text())
b = json.loads(a.board_report.read_text())
expected = f['count']
checks = {
    'execution': b['status'] == 'PASS',
    'complete_test_set': b['labelled_count'] == b['count'] == expected,
    'unique_inputs': len({r['input'] for r in b['results']}) == expected,
    'float_accuracy': f['top1'] >= .8,
    'board_accuracy': b['top1'] >= .8,
    'quantization_loss': f['top1'] - b['top1'] <= .02,
    'accelerator_execution': any(e['type'] in (3002, 3004) and e['value'] > 0 for e in b['qnn_profile']),
}
result = {'passed': all(checks.values()), 'checks': checks,
          'float_top1': f['top1'], 'board_top1': b['top1'],
          'drop_percentage_points': 100*(f['top1']-b['top1']), 'model_sha256': b['model_sha256']}
a.output.write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
raise SystemExit(0 if result['passed'] else 1)
