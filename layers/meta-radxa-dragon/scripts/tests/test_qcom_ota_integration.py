# SPDX-License-Identifier: MIT
"""Real OSTree/GPG tests. Set QCOM_OTA_INTEGRATION=1 with native tools on PATH."""
import functools
import http.server
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from unittest.mock import patch

from test_qcom_ota import ota, identity, LAYER

spec = importlib.util.spec_from_file_location('release', LAYER / 'scripts/qcom-ota-release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


@unittest.skipUnless(os.environ.get('QCOM_OTA_INTEGRATION') == '1', 'enable explicitly with real OSTree/GPG tools')
class SignedRepositoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='qcom-ota-integration-')
        cls.root = Path(cls.temp.name)
        cls.gpg = cls.root / 'keys'
        cls.gpg.mkdir(mode=0o700)
        ota.run('gpg', '--homedir', cls.gpg, '--batch', '--passphrase', '', '--quick-generate-key',
                'QCOM OTA isolated test', 'ed25519', 'sign', '0')
        keys = ota.run('gpg', '--homedir', cls.gpg, '--with-colons', '--list-keys')
        cls.key = next(line.split(':')[9] for line in keys.splitlines() if line.startswith('fpr:'))
        cls.repo = cls.root / 'source'
        ota.run('ostree', f'--repo={cls.repo}', 'init', '--mode=archive-z2')
        cls.commits = {}
        for ver in ('1.0.0', '1.1.0', '1.2.0'):
            tree = cls.root / ('tree-' + ver)
            (tree / 'usr/share/qcom-ota').mkdir(parents=True)
            (tree / 'usr/etc').mkdir(parents=True)
            (tree / 'usr/lib/modules/test').mkdir(parents=True)
            (tree / 'usr/etc/os-release').write_text('ID=qcom\nNAME=QCOM\nVERSION_ID=' + ver + '\n')
            (tree / 'usr/lib/modules/test/vmlinuz').write_text('fixture kernel ' + ver)
            (tree / 'usr/lib/modules/test/initramfs.img').write_text('fixture initramfs')
            (tree / 'usr/etc/keep.conf').write_text('factory=yes\n')
            (tree / 'usr/share/qcom-ota/identity.json').write_text(json.dumps(identity(version=ver)))
            checksum = ota.run('ostree', f'--repo={cls.repo}', 'commit', f'--branch={identity()["ref"]}', f'--tree=dir={tree}',
                               f'--add-metadata-string=qcom.ota={json.dumps(identity(version=ver))}',
                               f'--add-metadata-string=version={ver}', '--subject=fixture')
            cls.commits[ver] = checksum
            manifest = cls.root / f'{ver}.json'
            manifest.write_text(json.dumps(dict(identity(version=ver), commit=checksum)))
            release.publish(cls.repo, manifest, cls.root / ver, cls.key, cls.gpg)
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(cls.root))
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()
        ota.run('gpgconf', '--homedir', cls.gpg, '--kill', 'all')
        cls.temp.cleanup()

    def client(self, path, generation):
        ota.run('ostree', f'--repo={path}', 'init', '--mode=bare-user')
        url = f'http://127.0.0.1:{self.server.server_port}/{generation}/repo'
        ota.run('ostree', f'--repo={path}', 'remote', 'add', '--set=gpg-verify=true',
                '--set=gpg-verify-summary=true', f'--gpg-import={self.root / generation / "release-key.asc"}', 'qcom', url)

    def test_factory_manifest_rejects_stale_commit_metadata(self):
        import textwrap
        text = (LAYER / 'classes/qcom_ota_image.bbclass').read_text()
        body = text.split('python qcom_ota_manifest() {', 1)[1].split('\n}', 1)[0]
        class BitBake:
            @staticmethod
            def fatal(message):
                raise RuntimeError(message)
        scope = {'bb': BitBake}
        exec('def build(d):\n' + textwrap.indent(textwrap.dedent(body), '    '), scope)
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            work = Path(directory)
            values = dict(OSTREE_ROOTFS=str(self.root / 'tree-1.0.0'), WORKDIR=directory,
                          OSTREE_REPO=str(self.repo), IMGDEPLOYDIR=directory, IMAGE_LINK_NAME='factory')
            class Data:
                def getVar(self, name):
                    return values.get(name)
            (work / 'ostree_manifest').write_text(self.commits['1.0.0'])
            scope['build'](Data())
            output = work / 'factory.ota.json'
            self.assertEqual(json.loads(output.read_text())['commit'], self.commits['1.0.0'])
            output.unlink()
            bad = ota.run('ostree', f'--repo={self.repo}', 'commit', '--branch=bad-factory-metadata',
                          f'--tree=ref={self.commits["1.0.0"]}', '--add-metadata-string=qcom.ota={', '--subject=bad-metadata')
            (work / 'ostree_manifest').write_text(bad)
            with self.assertRaisesRegex(RuntimeError, 'malformed OTA metadata'):
                scope['build'](Data())
            self.assertFalse(output.exists())

    def test_signed_http_check_and_full_pull(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            repo = Path(directory) / 'repo'
            self.client(repo, '1.1.0')
            with patch.object(ota, 'REPO', repo), patch.object(ota, 'record'), \
                 patch.object(ota, 'booted', return_value=dict(checksum=self.commits['1.0.0'])):
                checksum, target = ota.check(identity())
                self.assertEqual(checksum, self.commits['1.1.0'])
                ota.ostree('pull', 'qcom', checksum)
                self.assertEqual(json.loads(ota.ostree('cat', checksum, '/usr/share/qcom-ota/identity.json')), target)

    def test_unsigned_summary_is_rejected(self):
        bad = self.root / 'unsigned'
        shutil.copytree(self.root / '1.1.0', bad)
        (bad / 'repo/summary.sig').unlink()
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            repo = Path(directory) / 'repo'
            self.client(repo, 'unsigned')
            with patch.object(ota, 'REPO', repo), self.assertRaises(ota.OTAError):
                ota.ostree('pull', 'qcom', identity()['ref'])

    def test_wrong_trust_key_is_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            repo = Path(directory) / 'repo'
            self.client(repo, '1.1.0')
            (repo / 'qcom.trustedkeys.gpg').unlink()
            with patch.object(ota, 'REPO', repo), self.assertRaises(ota.OTAError):
                ota.ostree('pull', 'qcom', identity()['ref'])

    def test_release_rejects_tampered_manifest(self):
        bad = dict(identity(version='9.0.0'), commit=self.commits['1.1.0'])
        path = self.root / 'tampered.json'
        path.write_text(json.dumps(bad))
        with self.assertRaises(release.ota.OTAError):
            release.publish(self.repo, path, self.root / 'bad-release', self.key, self.gpg)
        self.assertFalse((self.root / 'bad-release').exists())

    @unittest.skipUnless(os.geteuid() == 0, 'run under unshare -Ur for isolated deployment tests')
    def test_native_deploy_emits_versioned_uki(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            work = Path(directory)
            tree = work / 'tree'
            shutil.copytree(self.root / 'tree-1.0.0', tree)
            modules = tree / 'usr/lib/modules/test'
            (modules / 'uki.efi').hardlink_to(modules / 'vmlinuz')
            (modules / 'initramfs.img').write_bytes(b'')
            checksum = ota.run('ostree', f'--repo={self.repo}', 'commit', '--branch=uki-factory',
                               f'--tree=dir={tree}', '--subject=uki-fixture')
            root = work / 'sysroot'
            root.mkdir()
            ota.run('ostree', 'admin', f'--sysroot={root}', 'init-fs', '--modern', root)
            ota.run('ostree', 'admin', f'--sysroot={root}', 'os-init', 'qcom')
            repo = root / 'ostree/repo'
            ota.run('ostree', f'--repo={repo}', 'config', 'set', 'sysroot.bootloader', 'none')
            ota.run('ostree', f'--repo={repo}', 'pull-local', self.repo, checksum)
            ota.run('ostree', 'admin', f'--sysroot={root}', 'deploy', '--os=qcom', checksum)
            entries = list((root / 'boot/loader/entries').glob('*.conf'))
            self.assertEqual(len(entries), 1)
            fields = dict(line.split(maxsplit=1) for line in entries[0].read_text().splitlines() if line.strip())
            self.assertIn('uki', fields)
            self.assertNotIn('linux', fields)
            self.assertNotIn('initrd', fields)
            self.assertEqual((root / 'boot' / fields['uki'].lstrip('/')).read_bytes(),
                             (modules / 'uki.efi').read_bytes())

    @unittest.skipUnless(os.geteuid() == 0, 'run under unshare -Ur for isolated deployment tests')
    def test_client_installs_signed_checksum_into_real_sysroot(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory) / 'sysroot'
            root.mkdir()
            ota.run('ostree', 'admin', f'--sysroot={root}', 'init-fs', '--modern', root)
            ota.run('ostree', 'admin', f'--sysroot={root}', 'os-init', 'qcom')
            repo = root / 'ostree/repo'
            ota.run('ostree', f'--repo={repo}', 'config', 'set', 'sysroot.bootloader', 'none')
            checksum = self.commits['1.0.0']
            ota.run('ostree', f'--repo={repo}', 'pull-local', self.repo, checksum)
            ota.run('ostree', 'admin', f'--sysroot={root}', 'deploy', '--os=qcom', checksum)
            url = f'http://127.0.0.1:{self.server.server_port}/1.1.0/repo'
            original_run = ota.run

            def run_in_sysroot(*args):
                if args[:2] == ('ostree', 'admin'):
                    return original_run(*args[:2], f'--sysroot={root}', *args[2:])
                return original_run(*args)

            # Only the running-kernel identity and physical space checks are
            # substituted. Signature verification, pinning and deployment use
            # the real OSTree binary and an isolated real filesystem.
            with patch.object(ota, 'REPO', repo), patch.object(ota, 'STATE', root / 'state'), \
                 patch.object(ota, 'run', side_effect=run_in_sysroot), \
                 patch.object(ota, 'space_check'), \
                 patch.object(ota, 'booted', return_value=dict(checksum=checksum, index=0, pinned=False)):
                ota.configure(url, self.root / '1.1.0/release-key.asc')
                ota.install(identity())
                self.assertEqual(ota.deployments()[0]['checksum'], self.commits['1.1.0'])
                self.assertEqual(ota.read_json(root / 'state/state.json')['state'], 'reboot-pending')

    @unittest.skipUnless(os.geteuid() == 0, 'run under unshare -Ur for isolated deployment tests')
    def test_real_deploy_upgrade_and_rollback_preserve_configuration(self):
        with tempfile.TemporaryDirectory(dir=self.root) as directory:
            root = Path(directory) / 'sysroot'
            root.mkdir()
            ota.run('ostree', 'admin', f'--sysroot={root}', 'init-fs', '--modern', root)
            ota.run('ostree', 'admin', f'--sysroot={root}', 'os-init', 'qcom')
            repo = root / 'ostree/repo'
            ota.run('ostree', f'--repo={repo}', 'config', 'set', 'sysroot.bootloader', 'none')
            for ver in ('1.0.0', '1.1.0', '1.2.0'):
                checksum = self.commits[ver]
                ota.run('ostree', f'--repo={repo}', 'pull-local', self.repo, checksum)
                ota.run('ostree', 'admin', f'--sysroot={root}', 'deploy', '--os=qcom', '--retain', checksum)
                deployment = root / f'ostree/deploy/qcom/deploy/{checksum}.0'
                if ver == '1.0.0':
                    (deployment / 'etc/keep.conf').write_text('user=preserved\n')
                    (root / 'ostree/deploy/qcom/var/business-data').write_text('persistent\n')
                else:
                    self.assertEqual((deployment / 'etc/keep.conf').read_text(), 'user=preserved\n')
            ota.run('ostree', 'admin', f'--sysroot={root}', 'set-default', '1')
            status = json.loads(ota.run('ostree', 'admin', f'--sysroot={root}', 'status', '--json'))
            self.assertEqual(status['deployments'][0]['checksum'], self.commits['1.1.0'])
            self.assertEqual((root / 'ostree/deploy/qcom/var/business-data').read_text(), 'persistent\n')


if __name__ == '__main__':
    unittest.main()
