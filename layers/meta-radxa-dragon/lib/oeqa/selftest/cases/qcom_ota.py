# SPDX-License-Identifier: MIT
from pathlib import Path

from oeqa.selftest.case import OESelftestTestCase
from oeqa.utils.commands import get_bb_vars, runCmd


class QcomOTAMatrixTests(OESelftestTestCase):
    """Validate defaults across every MACHINE exported by the QCOM layer."""

    def setUp(self):
        super().setUp()
        layers = get_bb_vars(['BBLAYERS'])['BBLAYERS']
        if 'meta-qcom-distro' not in layers or 'meta-updater' not in layers:
            self.skipTest('OTA tests require the full qcom-distro layer composition; use audit-qcom-ota-matrix.py in the SDK')

    def test_all_machine_defaults(self):
        layer = Path(get_bb_vars(['LAYERDIR_qcom'])['LAYERDIR_qcom'])
        machines = sorted(p.stem for p in (layer / 'conf/machine').glob('*.conf'))
        self.assertTrue(machines)
        keys = ['QCOM_OTA_ENABLE', 'QCOM_OTA_BOOT_BACKEND', 'IMAGE_INSTALL', 'IMAGE_FSTYPES',
                'OSTREE_BOOTLOADER', 'OSTREE_BRANCHNAME', 'INITRAMFS_IMAGE', 'UKI_IMAGE_CLASS',
                'SOC_FAMILY', 'KERNEL_DEVICETREE', 'UKI_DEVICETREE']
        for machine in machines:
            with self.subTest(machine=machine):
                self.write_config(f'MACHINE = "{machine}"\nDISTRO = "qcom-distro"\n')
                values = get_bb_vars(keys, 'qcom-minimal-image')
                self.assertEqual(values['QCOM_OTA_ENABLE'], '1')
                self.assertIn('qcom-ota', values['IMAGE_INSTALL'].split())
                self.assertIn('ota-ext4', values['IMAGE_FSTYPES'].split())
                self.assertNotIn('ext4', values['IMAGE_FSTYPES'].split())
                self.assertNotIn('ostreepush', values['IMAGE_FSTYPES'].split())
                self.assertEqual(values['INITRAMFS_IMAGE'], 'initramfs-ostree-image')
                self.assertEqual(values['OSTREE_BRANCHNAME'], f'{machine}/qcom-minimal-image/stable')
                if machine == 'qcom-armv7a':
                    self.assertEqual(values['QCOM_OTA_BOOT_BACKEND'], 'fixed')
                    self.assertEqual(values['OSTREE_BOOTLOADER'], 'none')
                    self.assertNotIn('ota-esp', values['IMAGE_FSTYPES'].split())
                elif machine == 'radxa-dragon-q6a':
                    self.assertEqual(values['QCOM_OTA_BOOT_BACKEND'], 'embloader')
                    self.assertEqual(values['UKI_IMAGE_CLASS'], '')
                    self.assertEqual(values['SOC_FAMILY'], 'qcm6490')
                    self.assertEqual(values['KERNEL_DEVICETREE'].split(), ['qcom/qcs6490-radxa-dragon-q6a.dtb'])
                else:
                    self.assertEqual(values['QCOM_OTA_BOOT_BACKEND'], 'systemd-boot')
                    if machine == 'radxa-dragon-q8b':
                        self.assertEqual(values['SOC_FAMILY'], 'sc8280xp')
                        self.assertEqual(values['UKI_DEVICETREE'].split(), ['qcom/sc8280xp-radxa-dragon-q8b.dtb'])

    def test_opt_out_and_initramfs(self):
        self.write_config('MACHINE = "radxa-dragon-q6a"\nQCOM_OTA_ENABLE = "0"\n')
        values = get_bb_vars(['IMAGE_INSTALL', 'IMAGE_FSTYPES', 'WKS_FILE'], 'qcom-minimal-efi-sd-image')
        self.assertNotIn('qcom-ota', values['IMAGE_INSTALL'].split())
        self.assertNotIn('ota-ext4', values['IMAGE_FSTYPES'].split())
        self.assertEqual(values['WKS_FILE'], 'qcom-efi-sd.wks.in')
        self.write_config('MACHINE = "radxa-dragon-q6a"\nQCOM_OTA_ENABLE = "1"\n')
        for recipe in ('initramfs-ostree-image', 'initramfs-rootfs-image', 'esp-qcom-embloader-image'):
            values = get_bb_vars(['IMAGE_FSTYPES'], recipe)
            self.assertNotIn('ota-ext4', values['IMAGE_FSTYPES'].split())
            self.assertNotIn('qcomflash', values['IMAGE_FSTYPES'].split())

    def test_boot_hook_package_isolation(self):
        for machine in ('qcom-armv7a', 'radxa-dragon-q6a', 'radxa-dragon-q8b'):
            self.write_config(f'MACHINE = "{machine}"\nDISTRO = "qcom-distro"\n')
            values = get_bb_vars(['PACKAGE_ARCH', 'MACHINE_ARCH'], 'initramfs-framework')
            self.assertEqual(values['PACKAGE_ARCH'], values['MACHINE_ARCH'])

    def test_factory_and_target_ostree_features(self):
        self.write_config('MACHINE = "radxa-dragon-q8b"\nDISTRO = "qcom-distro"\n')
        for recipe in ('ostree', 'ostree-native'):
            values = get_bb_vars(['SRC_URI', 'PACKAGECONFIG'], recipe)
            for name in ('0001-Add-support-for-directories-instead-of-symbolic-link.patch',
                         '0002-Add-support-for-systemd-boot-bootloader.patch',
                         '0003-deploy-add-support-for-uki.patch'):
                self.assertIn('file://' + name, values['SRC_URI'].split())
            self.assertIn('gpgme', values['PACKAGECONFIG'].split())

    def test_sd_ufs_sources(self):
        self.write_config('MACHINE = "radxa-dragon-q6a"\n')
        for image in ('qcom-minimal-efi-sd-image', 'qcom-minimal-efi-ufs-4k-image'):
            values = get_bb_vars(['WKS_FILE', 'WIC_CREATE_EXTRA_ARGS'], image)
            self.assertEqual(values['WKS_FILE'], 'qcom-ota-sd.wks.in')
            if 'ufs' in image:
                self.assertIn('--sector-size 4096', values['WIC_CREATE_EXTRA_ARGS'])

    def test_offline_policy(self):
        layer = get_bb_vars(['LAYERDIR_qcom'])['LAYERDIR_qcom']
        runCmd(f'python3 -m unittest discover -s {layer}/scripts/tests -p test_qcom_ota.py -v')
