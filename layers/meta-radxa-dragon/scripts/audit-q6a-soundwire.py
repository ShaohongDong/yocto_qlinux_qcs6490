#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check a compiled Q6A DTB against the pre-change audio port baseline."""

import argparse
import json
import subprocess


def check_ports(get, baseline):
    for address, din, dout, extra in [('3210000', 1, 6, 2), ('3230000', 4, 0, 1)]:
        node = '/soc@0/soundwire@' + address
        for prop, count in [('qcom,din-ports', din), ('qcom,dout-ports', dout)]:
            if get(node, prop, False) != [count]:
                raise ValueError(f'{node}: incorrect {prop}')
        for prop, original in baseline[address].items():
            if get(node, prop, True) != original + [255] * extra:
                raise ValueError(f'{node}: changed or incomplete {prop}')
    for address, codec, prop, mapping in [
        ('3210000', 'codec@0,4', 'qcom,rx-port-mapping', [1, 2, 3, 4, 5]),
        ('3230000', 'codec@0,3', 'qcom,tx-port-mapping', [1, 1, 2, 3]),
    ]:
        node = f'/soc@0/soundwire@{address}/{codec}'
        if get(node, prop, False) != mapping:
            raise ValueError(f'{node}: codec mapping changed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dtb')
    parser.add_argument('--baseline', required=True)
    args = parser.parse_args()
    with open(args.baseline, encoding='utf-8') as stream:
        baseline = json.load(stream)

    def get(node, prop, byte):
        result = subprocess.check_output(
            ['fdtget', '-t', 'bx' if byte else 'x', args.dtb, node, prop], text=True)
        return [int(value, 16) for value in result.split()]

    check_ports(get, baseline)
    print('PASS: hardware port counts, parameter arrays and codec mappings')


if __name__ == '__main__':
    main()
