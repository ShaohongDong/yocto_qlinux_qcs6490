# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

LAYER = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('ota', LAYER / 'recipes-support/qcom-ota/files/qcom_ota.py')
ota = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ota)


def identity(**changes):
    value = dict(schema=1, machine='radxa-dragon-q6a', image='qcom-minimal-efi-sd-image',
                 channel='stable', ref='radxa-dragon-q6a/qcom-minimal-efi-sd-image/stable',
                 version='1.0.0', backend='embloader', kernel_fingerprint='a' * 64,
                 installed_bytes=100, boot_bytes=10, min_free_kib=1)
    value.update(changes)
    return value


class PolicyTests(unittest.TestCase):
    def test_upgrade_and_explicit_rollback(self):
        ota.compatible(identity(), identity(version='1.1.0'))
        ota.compatible(identity(version='2.0.0'), identity(), allow_rollback=True)
        with self.assertRaises(ota.OTAError):
            ota.compatible(identity(version='2.0.0'), identity())

    def test_no_cross_machine_image_channel_or_backend(self):
        for field, value in [('machine', 'radxa-dragon-q8b'), ('image', 'qcom-console-image'),
                             ('channel', 'candidate'), ('backend', 'fixed')]:
            target = identity(**{field: value})
            target['ref'] = '/'.join(target[k] for k in ('machine', 'image', 'channel'))
            with self.subTest(field=field), self.assertRaises(ota.OTAError):
                ota.compatible(identity(), target)

    def test_fixed_kernel_and_modules_must_match(self):
        current = identity(backend='fixed')
        ota.compatible(current, identity(backend='fixed', version='1.1.0'))
        with self.assertRaises(ota.OTAError):
            ota.compatible(current, identity(backend='fixed', kernel_fingerprint='b' * 64))

    def test_invalid_versions_and_paths(self):
        for value in ['1', '1.0', '01.0.0', '1.2.3;reboot', '../etc/passwd', '-1.2.3']:
            with self.subTest(value=value), self.assertRaises(ota.OTAError):
                ota.version(value)
        with self.assertRaises(ota.OTAError):
            ota.validate_identity(identity(ref='../other'))

    def test_url_requires_explicit_trust(self):
        for url in ['file:///etc', 'https://user:pass@example.com/repo', 'https://x/repo?key=x']:
            with self.subTest(url=url), self.assertRaises(ota.OTAError):
                ota.configure(url, '/unused')

    def test_atomic_state_and_lock(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(ota, 'STATE', Path(directory)):
            ota.write_json(Path(directory) / 'state.json', {'state': 'installing'})
            self.assertEqual(ota.read_json(Path(directory) / 'state.json')['state'], 'installing')
            with ota.locked(), self.assertRaises(ota.OTAError):
                with ota.locked():
                    self.fail('lock accepted a concurrent operation')

    def test_fixed_selector_points_at_complete_deployment(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / 'ostree/repo'
            repo.mkdir(parents=True)
            deployment = dict(checksum='a' * 64, serial=0)
            with patch.object(ota, 'REPO', repo), patch.object(ota.os, 'sync'):
                with self.assertRaises(ota.OTAError):
                    ota.fixed_select(deployment)
                target = repo.parent / ('deploy/qcom/deploy/' + 'a' * 64 + '.0')
                target.mkdir(parents=True)
                ota.fixed_select(deployment)
                self.assertEqual((repo.parent / 'qcom-active').resolve(), target)


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.active = dict(checksum='a' * 64, index=0, booted=True, pending=False)
        self.target = identity(version='1.1.0')

    def test_pending_update_blocks_second_install(self):
        with patch.object(ota, 'booted', return_value=self.active), \
             patch.object(ota, 'deployments', return_value=[dict(pending=True)]), \
             patch.object(ota, 'check') as check:
            with self.assertRaises(ota.OTAError):
                ota.install(identity())
            check.assert_not_called()

    def test_failed_download_never_changes_boot_selection(self):
        with patch.object(ota, 'booted', return_value=self.active), \
             patch.object(ota, 'deployments', return_value=[self.active]), \
             patch.object(ota, 'check', return_value=('b' * 64, self.target)), \
             patch.object(ota, 'space_check'), patch.object(ota, 'record'), \
             patch.object(ota, 'ostree', side_effect=ota.OTAError('network interrupted')), \
             patch.object(ota, 'run') as run:
            with self.assertRaises(ota.OTAError):
                ota.install(identity())
            run.assert_not_called()

    def test_install_uses_verified_checksum_and_does_not_reboot(self):
        with patch.object(ota, 'booted', return_value=self.active), \
             patch.object(ota, 'deployments', return_value=[self.active]), \
             patch.object(ota, 'check', return_value=('b' * 64, self.target)), \
             patch.object(ota, 'metadata', return_value=self.target), \
             patch.object(ota, 'space_check'), patch.object(ota, 'record') as record, \
             patch.object(ota, 'manage_pin'), \
             patch.object(ota, 'origin_file', return_value='/origin'), \
             patch.object(ota, 'ostree') as ostree, patch.object(ota, 'run') as run:
            ota.install(identity())
            ostree.assert_called_once_with('pull', 'qcom', 'b' * 64)
            self.assertEqual(run.call_args_list[-1].args[-1], 'b' * 64)
            self.assertNotIn('reboot', str(run.call_args_list))
            self.assertEqual(record.call_args.args[0], 'reboot-pending')

    def test_signature_verification_cannot_be_disabled(self):
        with patch.object(ota, 'ostree', return_value='false') as command:
            with self.assertRaises(ota.OTAError):
                ota.check(identity())
            self.assertEqual(command.call_count, 1)

    def test_failed_signature_never_reports_an_available_version(self):
        def command(*args):
            if args[0] == 'config':
                return 'true'
            raise ota.OTAError('GPG verification failed')
        with patch.object(ota, 'ostree', side_effect=command), patch.object(ota, 'record') as record:
            with self.assertRaises(ota.OTAError):
                ota.check(identity())
            record.assert_not_called()

    def test_space_failure_prevents_install(self):
        usage = type('Usage', (), {'free': 0})()
        with patch.object(ota.shutil, 'disk_usage', return_value=usage):
            with self.assertRaises(ota.OTAError):
                ota.space_check(identity(), self.target)

    def test_retention_does_not_remove_administrator_pins(self):
        active = dict(checksum='a' * 64, index=0, booted=True, pending=False, rollback=False, pinned=True)
        previous = dict(checksum='b' * 64, index=1, booted=False, pending=False, rollback=True, pinned=True)
        old = dict(checksum='c' * 64, index=2, booted=False, pending=False, rollback=False, pinned=True)
        administrator = dict(checksum='d' * 64, index=3, booted=False, pending=False, rollback=False, pinned=True)
        with tempfile.TemporaryDirectory() as directory, patch.object(ota, 'STATE', Path(directory)), \
             patch.object(ota, 'deployments', return_value=[active, previous, old, administrator]), \
             patch.object(ota, 'run') as run:
            ota.write_json(Path(directory) / 'managed-pins.json', ['a' * 64, 'b' * 64, 'c' * 64])
            ota.retain_successful_pair(active)
            self.assertEqual([call.args for call in run.call_args_list], [
                ('ostree', 'admin', 'pin', '--unpin', '2'), ('ostree', 'admin', 'undeploy', '2')])

    def test_failed_mount_health_check_never_blesses_boot(self):
        with patch.object(ota, 'booted', return_value=self.active), \
             patch.object(ota, 'read_json', return_value={'mounts': ['/sysroot'], 'services': []}), \
             patch.object(ota, 'run', side_effect=ota.OTAError('not mounted')) as run, \
             patch.object(ota, 'bless_embloader') as bless:
            with self.assertRaises(ota.OTAError):
                ota.confirm(identity())
            bless.assert_not_called()
            self.assertEqual(run.call_args.args[0], 'mountpoint')


class FixedInitramfsTests(unittest.TestCase):
    def test_prepare_root_receives_selector_symlink_and_rejects_traversal(self):
        source = (LAYER / 'recipes-core/initrdscripts/files/qcom-ota-fixed').read_text()
        # Exercise the real POSIX hook with the mount helper replaced by a
        # contract check: libostree requires its target to be a symlink.
        source = source.replace('/usr/lib/ostree/ostree-prepare-root', 'prepare_root_contract')
        contract = r'''prepare_root_contract() {
            [ "$2" = "ostree=/ostree/qcom-active" ] || return 10
            [ -L "$1${2#ostree=}" ] || return 11
        }
        ROOTFS_DIR=$1
        ostree_run
        '''
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = 'deploy/qcom/deploy/' + 'a' * 64 + '.0'
            (root / 'ostree' / target / 'usr').mkdir(parents=True)
            selector = root / 'ostree/qcom-active'
            selector.symlink_to(target)
            result = subprocess.run(['sh', '-c', source + '\n' + contract, 'hook', directory])
            self.assertEqual(result.returncode, 0)
            selector.unlink()
            selector.symlink_to('deploy/qcom/deploy/../deploy/' + 'a' * 64 + '.0')
            result = subprocess.run(['sh', '-c', source + '\n' + contract, 'hook', directory])
            self.assertNotEqual(result.returncode, 0)


class ImageIdentityTests(unittest.TestCase):
    def test_boot_space_counts_payload_and_fixed_fingerprint_covers_dtb(self):
        import textwrap
        text = (LAYER / 'classes/qcom_ota_image.bbclass').read_text()
        body = text.split('python qcom_ota_identity() {', 1)[1].split('\n}', 1)[0]
        scope = {}
        exec('def build(d):\n' + textwrap.indent(textwrap.dedent(body), '    '), scope)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'root'
            modules = root / 'usr/lib/modules/test'
            modules.mkdir(parents=True)
            (modules / 'vmlinuz').write_bytes(b'K' * 32)
            (modules / 'initramfs.img').write_bytes(b'I' * 8)
            (root / 'usr/etc').mkdir(parents=True)
            (root / 'usr/etc/fstab').write_text('/dev/old / ext4 defaults 0 1\nUUID=old /boot vfat defaults 0 2\n')
            deploy = Path(directory) / 'deploy'
            deploy.mkdir()
            dtb = deploy / 'board.dtb'
            dtb.write_bytes(b'device tree A')
            values = dict(OSTREE_ROOTFS=str(root), MACHINE='test-board', IMAGE_BASENAME='qcom-minimal-image',
                          QCOM_OTA_CHANNEL='stable', OSTREE_BRANCHNAME='test-board/qcom-minimal-image/stable',
                          QCOM_OTA_VERSION='1.0.0', QCOM_OTA_BOOT_BACKEND='embloader', QCOM_OTA_MIN_FREE_KIB='1',
                          DEPLOY_DIR_IMAGE=str(deploy), KERNEL_DEVICETREE='qcom/board.dtb')
            class Data:
                def getVar(self, name):
                    return values.get(name)
                def getVarFlag(self, name, flag):
                    return None
            manifest = root / 'usr/share/qcom-ota/identity.json'
            scope['build'](Data())
            self.assertEqual(json.loads(manifest.read_text())['boot_bytes'], 40)
            self.assertEqual((root / 'usr/etc/fstab').read_text(), 'LABEL=otaboot /boot vfat defaults 0 2\n')
            (modules / 'uki.efi').hardlink_to(modules / 'vmlinuz')
            values['QCOM_OTA_BOOT_BACKEND'] = 'systemd-boot'
            scope['build'](Data())
            self.assertEqual(json.loads(manifest.read_text())['boot_bytes'], 32)
            (modules / 'uki.efi').unlink()
            values['QCOM_OTA_BOOT_BACKEND'] = 'fixed'
            scope['build'](Data())
            first = json.loads(manifest.read_text())['kernel_fingerprint']
            dtb.write_bytes(b'device tree B')
            scope['build'](Data())
            second = json.loads(manifest.read_text())['kernel_fingerprint']
            self.assertNotEqual(first, second)
            values['KERNEL_CMDLINE_EXTRA'] = 'different=1'
            scope['build'](Data())
            self.assertNotEqual(second, json.loads(manifest.read_text())['kernel_fingerprint'])


class FactorySeedTests(unittest.TestCase):
    def test_efi_replacement_uses_existing_filename_and_blesses_factory_entry(self):
        import textwrap
        text = (LAYER / 'classes/qcom_ota_image.bbclass').read_text()
        body = text.split('python qcom_ota_boot_setup() {', 1)[1].split('\n}', 1)[0]
        scope = {}
        exec('def build(d):\n' + textwrap.indent(textwrap.dedent(body), '    '), scope)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'image/var').mkdir(parents=True)
            deployment = root / 'sysroot/ostree/deploy/qcom/deploy' / ('a' * 64 + '.0')
            (deployment / 'etc').mkdir(parents=True)
            (deployment / 'etc/fstab').write_text('UUID=old /boot vfat defaults 0 2\n')
            boot = root / 'ota-boot/boot'
            (boot / 'EFI/BOOT').mkdir(parents=True)
            (boot / 'EFI/BOOT/bootaa64.efi').write_bytes(b'old loader')
            (boot / 'loader/entries').mkdir(parents=True)
            (boot / 'loader/entries/ostree-1+3.conf').write_text('title factory\n')
            (root / 'deploy').mkdir()
            (root / 'deploy/embloader-0.7.efi').write_bytes(b'new embloader')
            values = dict(OTA_SYSROOT=str(root / 'sysroot'), OTA_BOOT=str(root / 'ota-boot'),
                          IMAGE_ROOTFS=str(root / 'image'), QCOM_OTA_BOOT_BACKEND='embloader',
                          DEPLOY_DIR_IMAGE=str(root / 'deploy'), EFIDIR='/EFI/BOOT', EFI_BOOT_IMAGE='bootaa64.efi')
            class Data:
                def getVar(self, name):
                    return values.get(name)
            scope['build'](Data())
            self.assertEqual(list((boot / 'EFI/BOOT').iterdir()), [boot / 'EFI/BOOT/bootaa64.efi'])
            self.assertEqual((boot / 'EFI/BOOT/bootaa64.efi').read_bytes(), b'new embloader')
            self.assertEqual(list((boot / 'loader/entries').iterdir()), [boot / 'loader/entries/ostree-1.conf'])
            self.assertIn('LABEL=otaboot /boot vfat', (deployment / 'etc/fstab').read_text())

    def test_existing_var_links_and_packaged_data_are_preserved(self):
        import textwrap
        text = (LAYER / 'classes/qcom_ota_image.bbclass').read_text()
        body = text.split('python qcom_ota_boot_setup() {', 1)[1].split('\n}', 1)[0]
        scope = {}
        exec('def build(d):\n' + textwrap.indent(textwrap.dedent(body), '    '), scope)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'image/var'
            target = root / 'sysroot/ostree/deploy/qcom/var'
            for var in (source, target):
                var.mkdir(parents=True)
                (var / 'run').symlink_to('../run')
                (var / 'tmp').symlink_to('volatile/tmp')
            (source / 'lib/product').mkdir(parents=True)
            (source / 'lib/product/seed').write_text('factory data')
            (source / 'lib/product/seed').chmod(0o600)
            deployment = target.parent / 'deploy' / ('a' * 64 + '.0')
            deployment.mkdir(parents=True)
            values = dict(OTA_SYSROOT=str(root / 'sysroot'), OTA_BOOT=str(root / 'boot'),
                          IMAGE_ROOTFS=str(root / 'image'), QCOM_OTA_BOOT_BACKEND='fixed')
            class Data:
                def getVar(self, name):
                    return values.get(name)
            scope['build'](Data())
            self.assertEqual((target / 'run').readlink(), Path('../run'))
            self.assertEqual((target / 'tmp').readlink(), Path('volatile/tmp'))
            self.assertEqual((target / 'lib/product/seed').read_text(), 'factory data')
            self.assertEqual((target / 'lib/product/seed').stat().st_mode & 0o777, 0o600)
            self.assertEqual((root / 'sysroot/ostree/qcom-active').resolve(), deployment)


class FixedPackageTests(unittest.TestCase):
    def test_factory_package_requires_initramfs_boot_images(self):
        text = (LAYER / 'classes/qcom_ota_image.bbclass').read_text()
        marker = 'create_qcomflash_pkg:prepend() {'
        body = text.split(marker, 1)[1].split('\n}', 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            deploy = root / 'deploy'
            deploy.mkdir()
            (deploy / 'boot-initramfs-board-a-qcom-armv7a.img').write_bytes(b'kernel+OTA initramfs A')
            (deploy / 'boot-initramfs-board-b-qcom-armv7a.img').write_bytes(b'kernel+OTA initramfs B')
            (deploy / 'boot-qcom-armv7a.img').write_bytes(b'kernel without initramfs')
            values = dict(QCOM_OTA_BOOT_BACKEND='fixed', KERNEL_DEVICETREE='qcom/board-a.dtb qcom/board-b.dtb',
                          DEPLOY_DIR_IMAGE=str(deploy), MACHINE='qcom-armv7a', QCOM_DTB_DEFAULT='board-b')
            for name, value in values.items():
                body = body.replace('${' + name + '}', value)
            script = 'set -e\nbbfatal() { echo "$*" >&2; exit 1; }\n' + body
            work = root / 'package'
            work.mkdir()
            subprocess.run(['sh', '-c', script], cwd=work, check=True)
            self.assertEqual((work / 'boot.img').read_bytes(), b'kernel+OTA initramfs B')
            self.assertEqual((work / 'boot-images/default-dtb').read_text(), 'board-b\n')
            self.assertTrue((work / 'boot-images/board-a.img').exists())
            (deploy / 'boot-initramfs-board-b-qcom-armv7a.img').unlink()
            result = subprocess.run(['sh', '-c', script], cwd=work, capture_output=True)
            self.assertNotEqual(result.returncode, 0)


class ConfirmationTests(unittest.TestCase):
    def test_embloader_confirmation_matches_running_deployment(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            repo = root / 'sysroot/ostree/repo'
            repo.mkdir(parents=True)
            active = dict(checksum='a' * 64, serial=0)
            target = repo.parent / ('deploy/qcom/deploy/' + 'a' * 64 + '.0')
            target.mkdir(parents=True)
            bootlink = repo.parent / 'boot.0/qcom/bootchecksum/0'
            bootlink.parent.mkdir(parents=True)
            bootlink.symlink_to(target)
            boot = root / 'boot'
            entries = boot / 'loader/entries'
            entries.mkdir(parents=True)
            entry = entries / 'ostree-2-qcom+2-1.conf'
            entry.write_text('options root=LABEL=otaroot ostree=/ostree/boot.0/qcom/bootchecksum/0\n')
            variable = root / 'efivar'
            with patch.object(ota, 'REPO', repo), patch.object(ota, 'BOOT', boot), \
                 patch.object(ota, 'BOOT_COUNT_PATH', variable), patch.object(ota.os, 'sync'):
                with self.assertRaises(ota.OTAError):
                    ota.bless_embloader(active)
                variable.write_bytes(b'\x06\0\0\0' + ('/loader/entries/' + entry.name + '\0').encode('utf-16-le'))
                with self.assertRaises(ota.OTAError):
                    ota.bless_embloader(dict(checksum='b' * 64, serial=0))
                self.assertTrue(entry.exists())
                ota.bless_embloader(active)
                self.assertFalse(entry.exists())
                self.assertTrue((entries / 'ostree-2-qcom.conf').exists())
                ota.bless_embloader(active)
                variable.unlink()
                ota.bless_embloader(active)

    def test_native_bless_failure_does_not_enable_upstream_bless(self):
        def command(*args):
            if args[0].endswith('/systemd-bless-boot'):
                raise ota.OTAError('ESP write failed')
            return ''
        with patch.object(ota, 'booted', return_value=dict(checksum='a' * 64)), \
             patch.object(ota, 'read_json', return_value={'mounts': [], 'services': []}), \
             patch.object(ota, 'run', side_effect=command), patch.object(ota, 'atomic_write') as write:
            with self.assertRaises(ota.OTAError):
                ota.confirm(identity(backend='systemd-boot'))
            write.assert_not_called()


class EmbloaderTests(unittest.TestCase):
    def test_real_counter_parser(self):
        # Extract the actual added header from the source patch and compile it
        # with the host compiler. Exercise the same parser used before boot.
        patch_text = (LAYER / 'recipes-bsp/embloader/files/0001-sdboot-count-ota-attempts.patch').read_text()
        start = patch_text.index('+++ b/embloader/include/qcom-bootcount.h')
        section = patch_text[start:].split('\n--- ', 1)[0]
        header = '\n'.join(line[1:] for line in section.splitlines() if line.startswith('+') and not line.startswith('+++'))
        program = '''
#include <assert.h>
#include "qcom-bootcount.h"
int main(void) {
 unsigned l=0, d=0; size_t s=0;
 assert(qcom_bootcount_parse("ostree-1-qcom", &l,&d,&s)==0);
 assert(qcom_bootcount_parse("ostree-2-qcom+3", &l,&d,&s)==1 && l==3 && d==0);
 assert(qcom_bootcount_parse("ostree-2-qcom+2-1", &l,&d,&s)==1 && l==2 && d==1);
 assert(qcom_bootcount_parse("ostree-2-qcom+0-3", &l,&d,&s)==1 && l==0 && d==3);
 assert(qcom_bootcount_parse("ostree-2-qcom-测试+3", &l,&d,&s)==1 && s==strlen("ostree-2-qcom-测试"));
 assert(qcom_bootcount_parse("ostree-2-qcom+", &l,&d,&s)==-1);
 assert(qcom_bootcount_parse("ostree-2-qcom+3-bad", &l,&d,&s)==-1);
 assert(qcom_bootcount_parse("ostree-2-qcom+99999999999999", &l,&d,&s)==-1);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / 'qcom-bootcount.h').write_text(header)
            (path / 'test.c').write_text(program)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
