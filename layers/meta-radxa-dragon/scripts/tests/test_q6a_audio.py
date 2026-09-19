# SPDX-License-Identifier: MIT
import copy
import importlib.util
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location(
    "q6a_audio", Path(__file__).resolve().parents[1] / "audit-q6a-audio.py")
audio = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audio)


class AudioAuditTests(unittest.TestCase):
    def test_usb_card_does_not_select_wrong_codec(self):
        cards = audio.card_records(""" 0 [Camera         ]: USB-Audio - 1080P USB Camera
                      USB camera at usb-port
 1 [QCS6490RadxaDra]: qcs6490 - QCS6490-Radxa-Dragon-Q6A
                      RadxaComputerCo.Ltd.-RadxaDragonQ6A-1.0
""")
        board = next(c for c in cards if c['name'] == 'QCS6490-Radxa-Dragon-Q6A')
        self.assertEqual(board['index'], 1)
        self.assertEqual(board['id'], 'QCS6490RadxaDra')
        self.assertEqual(audio.suffixes(board['long_name']), [
            'RadxaComputerCo.Ltd._RadxaDragonQ6A_1.0',
            'RadxaComputerCo.Ltd._RadxaDragonQ6A'])
        cards = audio.card_records(""" 2 [QCS6490RadxaDra]: qcs6490 - QCS6490-Radxa-Dragon-Q6A
                      RadxaComputerCo.Ltd.-RadxaDragonQ6A-1.0
""")
        self.assertEqual(cards[0]['index'], 2)

    def fixture(self):
        return dict(schema_version=1, board={'index': 1},
                    virtual_cards=['qcm6490virtualsndcard'],
                    acdb={'/etc/acdbdata/Q6A': {'acdb_cal.acdb': 'hash'}},
                    pal_configs={'/etc/mixer_paths_Q6A.xml': 'hash',
                                 '/etc/resourcemanager_Q6A.xml': 'hash'},
                    sinks={'returncode': 0, 'stdout': '[{"name":"pal_sink_headset_ll", "driver":"PipeWire", "properties":{"node.virtual":"true"}}]'})

    def test_pal_virtual_output_is_not_dummy(self):
        self.assertEqual(audio.issues(self.fixture()), [])

    def test_missing_acdb_wrong_platform_and_dummy_fail(self):
        data = self.fixture()
        data['virtual_cards'] = ['X1E80100virtualsndcard']
        data['acdb'] = {'/etc/acdbdata/Q6A': {}}
        data['sinks']['stdout'] = '[{"name":"auto_null", "driver":"PipeWire"}]'
        self.assertEqual(len(audio.issues(data)), 3)

    def test_unplugged_pal_sink_does_not_pass(self):
        data = self.fixture()
        data['sinks']['stdout'] = '[{"name":"pal_sink_headset_ll", "properties":{"q6a.jack.connected":"false"}}]'
        self.assertTrue(audio.issues(data))

    def test_failed_pulse_query_does_not_pass(self):
        data = copy.deepcopy(self.fixture())
        data['sinks']['returncode'] = 1
        self.assertTrue(audio.issues(data))
        data['sinks']['stdout'] = ''
        self.assertTrue(audio.issues(data))

    def test_missing_pal_config_fails(self):
        data = self.fixture()
        data['pal_configs'] = {}
        self.assertEqual(len(audio.issues(data)), 2)


if __name__ == '__main__':
    unittest.main()
