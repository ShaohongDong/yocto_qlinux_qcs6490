#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Exercise null-sink detection and reconnect against isolated PipeWire servers."""
import os
from pathlib import Path
import selectors
import subprocess
import sys
import tempfile
import time

with tempfile.TemporaryDirectory(prefix="music-output-test-") as directory:
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, PIPEWIRE_RUNTIME_DIR=directory,
               PULSE_SERVER=f"unix:{directory}/pulse/native",
               DBUS_SESSION_BUS_ADDRESS=f"unix:path={directory}/no-session-bus")
    env.pop("PIPEWIRE_REMOTE", None)
    processes = []
    with open(Path(directory) / "server.log", "w+") as log:
        try:
            def launch(command):
                process = subprocess.Popen(command, env=env, stdout=log, stderr=log)
                processes.append(process)
                return process
            launch([sys.argv[2]])
            pulse = launch([sys.argv[3]])
            probe = subprocess.Popen([sys.argv[1], "--watch"], env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processes.append(probe)
            selector = selectors.DefaultSelector()
            selector.register(probe.stdout, selectors.EVENT_READ)
            def expect(value):
                if not selector.select(8):
                    raise RuntimeError("timed out waiting for " + value)
                line = probe.stdout.readline().strip()
                if line != value:
                    raise RuntimeError(f"expected {value}, received {line}")
            expect("CONNECTED_NULL")
            pulse.terminate(); pulse.wait(timeout=5)
            expect("DISCONNECTED")
            launch([sys.argv[3]])
            expect("CONNECTED_NULL")
            if probe.wait(timeout=3):
                raise RuntimeError(probe.stderr.read())
            print("PASS: real null sink, disconnect, reconnect, null sink remains rejected")
        except Exception:
            log.seek(0); print(log.read(), file=sys.stderr)
            raise
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
