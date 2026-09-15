# SPDX-License-Identifier: MIT
import importlib.machinery
import importlib.util
from pathlib import Path
import socket
import tempfile
import unittest
from unittest.mock import patch

loader = importlib.machinery.SourceFileLoader('native_launcher', str(Path(__file__).resolve().parents[1]/'scripts/imx708-camera-launch'))
spec = importlib.util.spec_from_loader(loader.name, loader)
launch = importlib.util.module_from_spec(spec)
loader.exec_module(launch)


class StartupTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def test_only_actual_socket_is_selected(self):
        (self.root/'wayland-0.lock').touch()
        with socket.socket(socket.AF_UNIX) as sock:
            sock.bind(str(self.root/'wayland-1')); sock.listen(4)
            self.assertEqual(launch.display_socket(self.root),str(self.root/'wayland-1'))
            self.assertIsNone(launch.display_socket(self.root,'missing'))

    def test_timeout_still_returns_display_for_error_ui(self):
        with patch.object(launch,'display_socket',return_value='/run/user/1000/wayland-1'):
            self.assertEqual(launch.wait_ready(self.root,None,seconds=0,ready=lambda:False),'/run/user/1000/wayland-1')

    def test_ready_requires_sensor_and_receiver(self):
        p=self.root/'v4l-subdev0';p.mkdir();(p/'name').write_text('imx708 20-001a\n')
        self.assertFalse(launch.camera_ready(self.root))
        p=self.root/'video0';p.mkdir();(p/'name').write_text('msm_vfe0_video0\n')
        self.assertTrue(launch.camera_ready(self.root))
        (self.root/'v4l-subdev0'/'name').write_text('imx708\n')
        self.assertTrue(launch.camera_ready(self.root))
        (self.root/'v4l-subdev0'/'name').write_text('imx708-other\n')
        self.assertFalse(launch.camera_ready(self.root))

    def test_no_display_exits_without_starting_gui(self):
        with patch.object(launch,'wait_ready',return_value=None),patch.object(launch.os,'execv') as execute:
            self.assertEqual(launch.main(),1);execute.assert_not_called()


if __name__ == '__main__':unittest.main()
