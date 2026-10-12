#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.machinery
import importlib.util
from pathlib import Path
import unittest

loader = importlib.machinery.SourceFileLoader(
    "driver_trial", str(Path(__file__).parents[1] / "scripts/q6a-openhd-driver-trial"))
module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(module)


class ReadinessTests(unittest.TestCase):
    def test_delayed_discovery_and_transient_unmanaged(self):
        seen = []
        ticks = [0.0]
        attempts = [0]

        def invoke(argv):
            seen.append(argv)
            if "GENERAL.NM-MANAGED" in argv:
                return "no" if ticks[0] == 0 else "yes"
            if "connection" in argv:
                attempts[0] += 1
                if attempts[0] == 1:
                    raise RuntimeError("device is strictly unmanaged")
            if "GENERAL.CON-UUID" in argv:
                return "saved-uuid"
            return ""

        def pause(seconds):
            ticks[0] += seconds

        module.activate_saved_connection("wlan0", "saved-uuid", invoke,
                                         lambda: ticks[0], pause)
        self.assertEqual(attempts[0], 2)
        self.assertEqual(seen[0], ["udevadm", "settle", "--timeout=10"])
        self.assertTrue(all("restart" not in argv for argv in seen))

    def test_permanent_unmanaged_times_out(self):
        ticks = [0.0]

        def pause(seconds):
            ticks[0] += seconds

        with self.assertRaisesRegex(RuntimeError, "readiness timeout"):
            module.activate_saved_connection("wlan0", "saved-uuid", lambda _: "no",
                                             lambda: ticks[0], pause)
        self.assertEqual(ticks[0], 30)

    def test_activation_failure_does_not_retry_bad_credentials(self):
        def invoke(argv):
            if "GENERAL.NM-MANAGED" in argv:
                return "yes"
            if "connection" in argv:
                raise RuntimeError("secrets missing")
            return ""

        with self.assertRaisesRegex(RuntimeError, "secrets missing"):
            module.activate_saved_connection("wlan0", "saved-uuid", invoke)


if __name__ == "__main__":
    unittest.main()
