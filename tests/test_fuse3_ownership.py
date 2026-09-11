# SPDX-License-Identifier: MIT
"""Exercise ownership hooks with and without installed ptests."""

import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APPEND = ROOT / "layers/meta-radxa-dragon/dynamic-layers/openembedded-layer/recipes-support/fuse/fuse3_%.bbappend"


class FuseOwnershipTests(unittest.TestCase):
    def test_optional_directory_and_chown_failure(self):
        source = APPEND.read_text()
        for function, variable in (("do_install_ptest_base:append", "D"),
                                   ("fix_fuse3_ptest_parent_ownership", "PKGD")):
            with self.subTest(function=function), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                target = root / "package/usr/lib/fuse3"
                called = root / "called"
                body = re.search(re.escape(function) + r"\(\) \{\n(.*?)\n\}", source, re.S).group(1)
                body = body.replace("${" + variable + "}", str(root / "package"))
                body = body.replace("${libdir}", "/usr/lib").replace("${BPN}", "fuse3")
                script = 'set -e\nchown() { printf "%s\\n" "$@" > "$CALL_LOG"; return "$CHOWN_STATUS"; }\n' + body
                environment = dict(os.environ, CALL_LOG=str(called), CHOWN_STATUS="0")
                subprocess.run(["sh", "-c", script], env=environment, check=True)
                self.assertFalse(called.exists())
                target.mkdir(parents=True)
                subprocess.run(["sh", "-c", script], env=environment, check=True)
                self.assertEqual(called.read_text().splitlines(), ["root:root", str(target)])
                environment["CHOWN_STATUS"] = "1"
                self.assertNotEqual(subprocess.run(["sh", "-c", script], env=environment).returncode, 0)


if __name__ == "__main__":
    unittest.main()
