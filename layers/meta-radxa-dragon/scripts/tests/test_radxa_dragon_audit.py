# SPDX-License-Identifier: MIT
"""Regression checks for Q6A boot and compressed-module audit failures."""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from radxa_dragon_audit import parse_bls, validate_module_dependencies


class ModuleDependenciesTests(unittest.TestCase):
    complete = """kernel/net/wireless/cfg80211.ko.zst:
kernel/net/bluetooth/bluetooth.ko.zst:
kernel/fs/autofs/autofs4.ko.zst:
updates/dkms/aic_load_fw.ko:
updates/dkms/aic8800_fdrv.ko: kernel/net/wireless/cfg80211.ko.zst updates/dkms/aic_load_fw.ko
updates/dkms/aic_btusb.ko: kernel/net/bluetooth/bluetooth.ko.zst
"""

    def test_compressed_dependencies(self):
        self.assertEqual(len(validate_module_dependencies(self.complete)), 6)

    def test_original_three_line_depmod_failure(self):
        original = """updates/dkms/aic_load_fw.ko:
updates/dkms/aic8800_fdrv.ko: updates/dkms/aic_load_fw.ko
updates/dkms/aic_btusb.ko:
"""
        with self.assertRaisesRegex(RuntimeError, "incomplete module dependencies"):
            validate_module_dependencies(original)

    def test_dependency_must_be_indexed(self):
        with self.assertRaisesRegex(RuntimeError, "unindexed module dependency"):
            validate_module_dependencies(self.complete.replace(
                "kernel/net/bluetooth/bluetooth.ko.zst:\n", ""))

    def test_missing_autofs(self):
        with self.assertRaisesRegex(RuntimeError, "autofs4"):
            validate_module_dependencies(self.complete.replace(
                "kernel/fs/autofs/autofs4.ko.zst:\n", ""))


class BlsTests(unittest.TestCase):
    def test_spaces_in_title_and_options(self):
        fields = parse_bls("# boot entry\ntitle Radxa Dragon Q6A\noptions root=PARTLABEL=rootfs rw\n")
        self.assertEqual(fields["title"], "Radxa Dragon Q6A")
        self.assertEqual(fields["options"], "root=PARTLABEL=rootfs rw")

    def test_ambiguous_payload_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "duplicate BLS field"):
            parse_bls("linux /Image\nlinux /stale-Image\n")


if __name__ == "__main__":
    unittest.main()
