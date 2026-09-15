# SPDX-License-Identifier: MIT
"""CLI contract tests use an absent private system bus, never the host Wi-Fi."""
import json
import os
import pty
import select
import signal
import subprocess
import sys
import termios
import unittest

APP = sys.argv.pop(1)


class CliTests(unittest.TestCase):
    def run_app(self, *args, input=None):
        env = dict(os.environ, DBUS_SYSTEM_BUS_ADDRESS="unix:path=/nonexistent/wifi-test-bus")
        return subprocess.run([APP, *args], input=input, text=True, capture_output=True, env=env, timeout=8)

    def test_self_test_without_display_or_bus(self):
        self.assertEqual(self.run_app("--self-test").returncode, 0)

    def test_missing_service(self):
        result = self.run_app("status", "--json")
        self.assertEqual(result.returncode, 1)
        self.assertFalse(json.loads(result.stdout)["ok"])

    def test_invalid_arguments(self):
        for args in [("ping", "--target", "-c"), ("scan", "--timeout", "1x"),
                     ("connect", "--ssid", "x", "--hidden"), ("scan", "--interface"),
                     ("unknown",), ("connect", "--ssid", "x", "--password", "secret")]:
            with self.subTest(args=args):
                result = self.run_app(*args, "--json")
                self.assertEqual(result.returncode, 2)
                self.assertFalse(json.loads(result.stdout)["ok"])
                self.assertNotIn("secret", result.stdout + result.stderr)

    def test_password_stdin_not_reported(self):
        secret = "TEST-ONLY-secret-'\\\"-$"
        result = self.run_app("connect", "--ssid", "Test: \\\"", "--password-stdin", "--json", input=secret + "\n")
        self.assertEqual(result.returncode, 1)
        json.loads(result.stdout)
        self.assertNotIn(secret, result.stdout + result.stderr)

    def test_open_network_does_not_read_password(self):
        result = self.run_app("connect", "--ssid", "Test", "--security", "open", "--json")
        self.assertEqual(result.returncode, 1)

    def test_password_prompt_interrupt_restores_echo(self):
        master, slave = pty.openpty()
        process = subprocess.Popen([APP, "connect", "--ssid", "Test"], stdin=slave, stdout=slave, stderr=slave)
        try:
            self.assertTrue(select.select([master], [], [], 3)[0])
            os.read(master, 4096)
            self.assertFalse(termios.tcgetattr(slave)[3] & termios.ECHO)
            process.send_signal(signal.SIGINT)
            process.wait(timeout=3)
            self.assertTrue(termios.tcgetattr(slave)[3] & termios.ECHO)
            self.assertEqual(process.returncode, 2)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main()
