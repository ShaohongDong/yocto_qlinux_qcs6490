#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.machinery
import importlib.util
from pathlib import Path
import unittest

loader = importlib.machinery.SourceFileLoader(
    "video_analysis", str(Path(__file__).parents[1] / "scripts/analyze-video"))
module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(module)


class VideoAnalysisTests(unittest.TestCase):
    def header(self):
        return {"frame_mbs_only_flag": 1, "vui_parameters_present_flag": 1,
                "timing_info_present_flag": 1, "fixed_frame_rate_flag": 1,
                "pic_struct_present_flag": 0,
                "num_units_in_tick": 2097152, "time_scale": 125829120}

    def test_large_clock_fields_indicate_30_frames_not_60(self):
        self.assertEqual(module.progressive_vui_rate(self.header()), 30)

    def test_interlaced_or_variable_structure_is_not_assumed_progressive(self):
        for name in ("frame_mbs_only_flag", "fixed_frame_rate_flag", "timing_info_present_flag"):
            header = self.header()
            header[name] = 0
            with self.assertRaisesRegex(ValueError, "picture structure"):
                module.progressive_vui_rate(header)

    def test_missing_or_zero_clock_is_not_accepted(self):
        header = self.header()
        header["num_units_in_tick"] = 0
        with self.assertRaisesRegex(ValueError, "clock fields"):
            module.progressive_vui_rate(header)

    def test_extradata_does_not_substitute_for_in_band_headers(self):
        trace = """[trace_headers @ 1] Sequence Parameter Set
[trace_headers @ 1] 3 nal_unit_type 00111 = 7
[trace_headers @ 1] Picture Parameter Set
[trace_headers @ 1] 3 nal_unit_type 01000 = 8
[trace_headers @ 1] Packet: 100 bytes, key frame.
[trace_headers @ 1] Slice Header
[trace_headers @ 1] 3 nal_unit_type 00101 = 5
"""
        sps, packets = module.parse_headers(trace)
        self.assertEqual(len(sps), 1)
        with self.assertRaisesRegex(ValueError, "missing in-band"):
            module.check_packet_headers(packets, [0])

    def test_headers_after_idr_do_not_satisfy_prepend_requirement(self):
        with self.assertRaisesRegex(ValueError, "missing in-band"):
            module.check_packet_headers([[9, 5, 7, 8]], [0])


if __name__ == "__main__":
    unittest.main()
