#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.machinery
import importlib.util
import unittest
from pathlib import Path

loader = importlib.machinery.SourceFileLoader(
    "monitor", str(Path(__file__).parents[1] / "scripts/q6a-openhd-monitor"))
module = importlib.util.module_from_spec(importlib.util.spec_from_loader(loader.name, loader))
loader.exec_module(module)


class MonitorTests(unittest.TestCase):
    def commands(self, fail=None):
        seen = []
        mode = ["managed"]

        def invoke(argv):
            seen.append(argv)
            if argv == ["iw", "dev", "wlan0", "info"]:
                return "Interface wlan0\n type " + mode[0] + "\n channel 36 (5180 MHz), width: 20 MHz\n"
            if argv == ["iw", "dev", "wlan0", "set", "type", "monitor"]:
                mode[0] = "monitor"
            if "GENERAL.STATE" in argv:
                return "10 (unmanaged)"
            if "GENERAL.NM-MANAGED" in argv:
                return "yes"
            if "GENERAL.CON-UUID" in argv:
                return "saved-uuid"
            if argv[:4] == ["busctl", "--system", "--timeout=2", "call"]:
                return 'o "/fi/w1/wpa_supplicant1/Interfaces/1"'
            if argv[:4] == ["busctl", "--system", "--timeout=2", "get-property"]:
                return "ao 0"
            if argv == fail:
                raise RuntimeError("injected failure")
            return ""

        return seen, invoke

    def assert_restored(self, seen):
        self.assertEqual(seen[-4:], [
            ["iw", "dev", "wlan0", "set", "type", "managed"],
            ["ip", "link", "set", "wlan0", "up"],
            ["nmcli", "device", "set", "wlan0", "managed", "yes"],
            ["nmcli", "connection", "up", "uuid", "saved-uuid", "ifname", "wlan0"],
        ])

    def test_restore_after_success_or_child_error(self):
        for result in (0, 1):
            seen, invoke = self.commands()
            self.assertEqual(module.with_monitor("wlan0", 5180, ["probe"], invoke, lambda _: result), result)
            self.assert_restored(seen)

    def test_restore_after_frequency_failure(self):
        seen, invoke = self.commands(["iw", "dev", "wlan0", "set", "freq", "5180", "HT20"])
        with self.assertRaises(RuntimeError):
            module.with_monitor("wlan0", 5180, ["probe"], invoke, lambda _: self.fail("must not execute"))
        self.assert_restored(seen)

    def test_rejects_supplicant_mode_reset_before_tx(self):
        seen, ordinary = self.commands()
        reads = [0]

        def invoke(argv):
            result = ordinary(argv)
            if argv == ["iw", "dev", "wlan0", "info"]:
                reads[0] += 1
                if reads[0] == 2:
                    return "Interface wlan0\n type managed\n"
            return result

        with self.assertRaisesRegex(RuntimeError, "readback does not match"):
            module.with_monitor("wlan0", 5180, ["probe"], invoke,
                                lambda _: self.fail("must not transmit after mode reset"))
        self.assert_restored(seen)

    def test_waits_for_unmanage_completion(self):
        ticks = [0.0]

        def pause(seconds):
            ticks[0] += seconds

        def invoke(_):
            return "10 (unmanaged)" if ticks[0] >= 0.2 else "110 (deactivating)"

        module.wait_unmanaged("wlan0", invoke, lambda: ticks[0], pause)
        self.assertGreaterEqual(ticks[0], 0.2)

    def test_unmanage_timeout_is_bounded(self):
        ticks = [0.0]

        def pause(seconds):
            ticks[0] += seconds

        with self.assertRaisesRegex(RuntimeError, "did not finish unmanaging"):
            module.wait_unmanaged("wlan0", lambda _: "110 (deactivating)",
                                  lambda: ticks[0], pause)
        self.assertLess(ticks[0], 10.2)

    def test_waits_for_target_supplicant_without_waiting_for_other_radios(self):
        ticks = [0.0]
        target = "/fi/w1/wpa_supplicant1/Interfaces/1"

        def pause(seconds):
            ticks[0] += seconds

        def invoke(_):
            return ('ao 2 "' + target + '" "/fi/w1/wpa_supplicant1/Interfaces/2"'
                    if ticks[0] < 0.2 else 'ao 1 "/fi/w1/wpa_supplicant1/Interfaces/2"')

        module.wait_supplicant_removed(target, invoke, lambda: ticks[0], pause)
        self.assertGreaterEqual(ticks[0], 0.2)

    def test_restores_only_idle_target_p2p_policy(self):
        seen, ordinary = self.commands()
        p2p_managed = [True]

        def invoke(argv):
            result = ordinary(argv)
            if argv == ["nmcli", "-t", "-f", "DEVICE,TYPE", "device", "status"]:
                return "wlan0:wifi\np2p-dev-wlan0:wifi-p2p\np2p-dev-wlan1:wifi-p2p"
            if "p2p-dev-wlan0" in argv:
                if argv[-2:] == ["managed", "no"]:
                    p2p_managed[0] = False
                if "GENERAL.STATE" in argv:
                    return "30 (disconnected)" if p2p_managed[0] else "10 (unmanaged)"
            return result

        self.assertEqual(module.with_monitor("wlan0", 5180, ["probe"], invoke, lambda _: 0), 0)
        self.assertEqual(seen[-1], ["nmcli", "device", "set", "p2p-dev-wlan0", "managed", "yes"])
        self.assertTrue(all("p2p-dev-wlan1" not in argv for argv in seen))

    def test_refuses_active_p2p_without_network_changes(self):
        seen, ordinary = self.commands()

        def invoke(argv):
            result = ordinary(argv)
            if argv == ["nmcli", "-t", "-f", "DEVICE,TYPE", "device", "status"]:
                return "p2p-dev-wlan0:wifi-p2p"
            if "GENERAL.STATE" in argv and "p2p-dev-wlan0" in argv:
                return "100 (connected)"
            return result

        with self.assertRaisesRegex(RuntimeError, "P2P device must be idle"):
            module.with_monitor("wlan0", 5180, ["probe"], invoke, lambda _: self.fail("must not execute"))
        self.assertTrue(all("set" not in argv and "disconnect" not in argv for argv in seen))

    def test_restore_after_interrupt(self):
        seen, invoke = self.commands()

        def cancel(_):
            raise KeyboardInterrupt

        with self.assertRaises(KeyboardInterrupt):
            module.with_monitor("wlan0", 5180, ["probe"], invoke, cancel)
        self.assert_restored(seen)

    def test_restore_attempts_continue_after_one_failure(self):
        seen, invoke = self.commands(["iw", "dev", "wlan0", "set", "type", "managed"])
        with self.assertRaisesRegex(RuntimeError, "restoration failed"):
            module.with_monitor("wlan0", 5180, ["probe"], invoke, lambda _: 0)
        self.assert_restored(seen)

    def test_restore_retries_transient_device_state(self):
        ticks = [0.0]
        calls = []

        def invoke(argv):
            calls.append(argv)
            if len(calls) == 1:
                raise RuntimeError("device is not available")
            return ""

        def pause(seconds):
            ticks[0] += seconds

        module.restore_connection("wlan0", "saved-uuid", invoke,
                                  lambda: ticks[0], pause)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0], calls[1])

    def test_reports_both_trial_and_restore_failure(self):
        seen, invoke = self.commands(["iw", "dev", "wlan0", "set", "type", "managed"])

        def child(_):
            raise OSError("probe could not start")

        with self.assertRaisesRegex(RuntimeError, "trial failed: probe could not start.*restoration failed"):
            module.with_monitor("wlan0", 5180, ["probe"], invoke, child)
        self.assert_restored(seen)


if __name__ == "__main__":
    unittest.main()
