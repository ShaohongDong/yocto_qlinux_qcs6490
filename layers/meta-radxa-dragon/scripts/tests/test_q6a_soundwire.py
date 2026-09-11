# SPDX-License-Identifier: MIT
import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('audit', ROOT / 'audit-q6a-soundwire.py')
AUDIT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(AUDIT)


class SoundWireAuditTests(unittest.TestCase):
    def setUp(self):
        self.baseline = json.loads((ROOT / 'tests/data/q6a-soundwire-baseline.json').read_text())
        self.props = {}
        for address, din, dout, extra in [('3210000', 1, 6, 2), ('3230000', 4, 0, 1)]:
            node = '/soc@0/soundwire@' + address
            self.props[node, 'qcom,din-ports'] = [din]
            self.props[node, 'qcom,dout-ports'] = [dout]
            for prop, values in self.baseline[address].items():
                self.props[node, prop] = values + [255] * extra
        self.props['/soc@0/soundwire@3210000/codec@0,4', 'qcom,rx-port-mapping'] = [1, 2, 3, 4, 5]
        self.props['/soc@0/soundwire@3230000/codec@0,3', 'qcom,tx-port-mapping'] = [1, 1, 2, 3]

    def check(self, props):
        AUDIT.check_ports(lambda node, prop, byte: props[node, prop], self.baseline)

    def test_complete_ports(self):
        self.check(self.props)

    def test_rejects_truncated_array(self):
        props = copy.deepcopy(self.props)
        props['/soc@0/soundwire@3210000', 'qcom,ports-offset1'].pop()
        with self.assertRaises(ValueError):
            self.check(props)

    def test_rejects_changed_mapping(self):
        props = copy.deepcopy(self.props)
        props['/soc@0/soundwire@3230000/codec@0,3', 'qcom,tx-port-mapping'][0] = 4
        with self.assertRaises(ValueError):
            self.check(props)

    def test_rejects_changed_existing_parameter(self):
        props = copy.deepcopy(self.props)
        props['/soc@0/soundwire@3230000', 'qcom,ports-offset1'][0] = 255
        with self.assertRaises(ValueError):
            self.check(props)


if __name__ == '__main__':
    unittest.main()
