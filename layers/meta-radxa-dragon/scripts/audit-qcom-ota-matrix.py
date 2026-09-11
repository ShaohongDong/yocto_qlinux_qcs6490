#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Parse the complete OTA MACHINE matrix in an already activated Qualcomm SDK."""
import argparse
import glob
import fnmatch
import os
import sys
import json
from pathlib import Path
import re
import subprocess
import tempfile


def inspect(bitbake, machine, image, enabled):
    with tempfile.NamedTemporaryFile(mode='w', suffix='.conf') as config:
        config.write(f'MACHINE = "{machine}"\nDISTRO = "qcom-distro"\nQCOM_OTA_ENABLE = "{enabled}"\n')
        config.write('BB_NUMBER_PARSE_THREADS = "4"\n')
        config.flush()
        result = subprocess.run([sys.executable, str(Path(__file__).resolve()), '--parse-worker',
                                 bitbake, config.name, image], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode:
        raise RuntimeError(result.stderr[-3000:] + result.stdout[-3000:])
    variables = json.loads(result.stdout.split('QCOM_OTA_VARIABLES=', 1)[1])
    if machine in ('radxa-dragon-q6a', 'radxa-dragon-q8b'):
        soc, dtb = ('qcm6490', 'qcom/qcs6490-radxa-dragon-q6a.dtb') if machine.endswith('q6a') else ('sc8280xp', 'qcom/sc8280xp-radxa-dragon-q8b.dtb')
        assert variables['QCOM_MACHINE_SOC_FAMILY'] == soc, 'MACHINE changed after the wrong machine configuration was loaded'
        assert variables['QCOM_MACHINE_DEVICETREE'].split() == [dtb], 'machine DTB selection mismatch'
    if enabled:
        assert not variables['SIGGEN_LOCKEDSIGS_TYPES'], 'old eSDK task signatures must not override OTA metadata'
    if enabled == 0:
        assert 'qcom-ota' not in variables['IMAGE_INSTALL'].split()
        assert 'ota-ext4' not in variables['IMAGE_FSTYPES'].split()
        assert variables['WKS_FILE'] == 'qcom-efi-sd.wks.in'
    elif image == 'initramfs-framework':
        assert variables['PACKAGE_ARCH'] == variables['MACHINE_ARCH'], 'boot hook package must be machine specific'
    elif image in ('ostree', 'ostree-native'):
        assert set(variables['SD_BOOT_PATCHES'].split()) <= set(variables['SRC_URI'].split()), 'factory and target OSTree must both support UKI/systemd-boot'
        assert 'gpgme' in variables['PACKAGECONFIG'].split(), 'OSTree signature verification is required'
    elif image.startswith(('initramfs-', 'esp-')):
        assert not {'ota-ext4', 'ota-esp', 'qcomflash'} & set(variables['IMAGE_FSTYPES'].split())
    else:
        assert variables['QCOM_OTA_ENABLE'] == '1'
        assert variables['KERNEL_DEVICETREE'].split() == variables['QCOM_MACHINE_DEVICETREE'].split(), 'OTA image discarded the configured DTB list'
        assert 'qcom-ota' in variables['IMAGE_INSTALL'].split()
        assert 'ota-ext4' in variables['IMAGE_FSTYPES'].split()
        assert not any(kind.startswith('ext4') for kind in variables['IMAGE_FSTYPES'].split()), 'plain rootfs published alongside OTA image'
        assert 'do_image_ext4' not in variables['__BBTASKS'], 'plain rootfs task was generated before OTA filtering'
        assert variables['OSTREE_COMMIT_SKIP_IF_UNCHANGED'] == '0', 'OTA commits must include current metadata even when their tree is unchanged'
        # Execute the parsed shell fragment: escaped literal quotes can pass
        # metadata parsing but split pretty JSON into multiple commit arguments.
        with tempfile.TemporaryDirectory(prefix='qcom-ota-metadata-') as directory:
            identity = Path(directory) / 'usr/share/qcom-ota/identity.json'
            identity.parent.mkdir(parents=True)
            encoded = json.dumps(dict(machine=machine, version='1.0.0'), indent=2)
            identity.write_text(encoded + '\n')
            fragment = variables['EXTRA_OSTREE_COMMIT'].replace(variables['OSTREE_ROOTFS'], directory)
            capture = "python3 -c 'import json,sys; print(json.dumps(sys.argv[1:]))' "
            result = subprocess.run(['sh', '-c', capture + fragment], check=True, text=True, capture_output=True)
            assert '--add-metadata-string=qcom.ota=' + encoded in json.loads(result.stdout), 'commit metadata shell quoting corrupts JSON'
        assert not {'ostreepush', 'garagesign', 'garagecheck'} & set(variables['IMAGE_FSTYPES'].split())
        assert variables['OSTREE_BRANCHNAME'] == f'{machine}/{image}/stable'
        backend = 'fixed' if machine == 'qcom-armv7a' else 'embloader' if machine == 'radxa-dragon-q6a' else 'systemd-boot'
        assert variables['QCOM_OTA_BOOT_BACKEND'] == backend
        if backend == 'fixed':
            assert variables['OSTREE_BOOTLOADER'] == 'none'
            assert 'ota-esp' not in variables['IMAGE_FSTYPES'].split()
        if backend == 'embloader':
            assert not variables['UKI_IMAGE_CLASS']
        if machine == 'radxa-dragon-q8b':
            assert variables['UKI_DEVICETREE'].split() == ['qcom/sc8280xp-radxa-dragon-q8b.dtb']
        if '-efi-' in image:
            assert variables['WKS_FILE'] == 'qcom-ota-sd.wks.in'
    return dict(machine=machine, image=image, enabled=enabled, status='passed',
                backend=variables.get('QCOM_OTA_BOOT_BACKEND'), evidence='bitbake-metadata')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bitbake', default='layers/oe-core/bitbake/bin/bitbake')
    parser.add_argument('--machines', nargs='*')
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    layer = Path(__file__).resolve().parents[1]
    machines = args.machines or sorted(p.stem for p in (layer / 'conf/machine').glob('*.conf'))
    cases = [(machine, 'qcom-minimal-image', 1) for machine in machines]
    cases += [('radxa-dragon-q6a', image, enabled) for image, enabled in [
        ('qcom-minimal-efi-sd-image', 1), ('qcom-minimal-efi-ufs-4k-image', 1),
        ('qcom-minimal-efi-sd-image', 0), ('initramfs-ostree-image', 1),
        ('initramfs-rootfs-image', 1), ('esp-qcom-embloader-image', 1)]]
    cases += [(machine, 'initramfs-framework', 1) for machine in
              ('qcom-armv7a', 'radxa-dragon-q6a', 'radxa-dragon-q8b')]
    cases += [(machine, recipe, 1) for machine in
              ('qcom-armv7a', 'radxa-dragon-q6a', 'radxa-dragon-q8b')
              for recipe in ('ostree', 'ostree-native')]
    results = []
    for machine, image, enabled in cases:
        if not re.fullmatch('[A-Za-z0-9_-]+', machine):
            parser.error('Invalid machine name')
        try:
            result = inspect(args.bitbake, machine, image, enabled)
        except (RuntimeError, AssertionError, KeyError) as error:
            result = dict(machine=machine, image=image, enabled=enabled, status='failed', error=str(error))
        results.append(result)
        print(json.dumps(result), flush=True)
        args.output.write_text(json.dumps(results, indent=2) + '\n')
    return int(any(result['status'] != 'passed' for result in results))


def parse_worker(bitbake, configfile, image):
    # Use BitBake's own config/recipe parsers without starting a cooker or
    # reparsing thousands of unrelated recipes for each MACHINE. This checks
    # expanded metadata, not provider resolution or a successful board build.
    sys.path.insert(0, str(Path(bitbake).resolve().parents[1] / 'lib'))
    import bb.cookerdata
    import bb.cache
    import bb.codeparser
    config = bb.cookerdata.CookerConfiguration()
    config.env = os.environ.copy()
    # MACHINE and DISTRO must be set before bitbake.conf includes their
    # configuration. A postfile only renames MACHINE after loading the old DTB.
    config.prefile = [configfile]
    builder = bb.cookerdata.CookerDataBuilder(config)
    builder.parseBaseConfiguration(worker=True)
    files = [path for pattern in builder.data.getVar('BBFILES').split() for path in glob.glob(pattern)]
    basename = 'ostree' if image == 'ostree-native' else image
    candidates = [path for path in files if Path(path).suffix == '.bb' and Path(path).stem.split('_')[0] == basename]
    if len(candidates) != 1:
        raise RuntimeError(f'Expected one active recipe for {image}: {candidates}')
    recipe = candidates[0]
    appends = [path for path in files if path.endswith('.bbappend') and
               fnmatch.fnmatch(Path(recipe).name, Path(path).name[:-6].replace('%', '*'))]
    virtual = 'virtual:native:' + recipe if image == 'ostree-native' else recipe
    data = builder.parseRecipe(virtual, appends, None)
    keys = ('QCOM_OTA_ENABLE', 'IMAGE_INSTALL', 'IMAGE_FSTYPES', 'WKS_FILE', 'OSTREE_BRANCHNAME',
            'QCOM_OTA_BOOT_BACKEND', 'OSTREE_BOOTLOADER', 'UKI_IMAGE_CLASS', 'PACKAGE_ARCH', 'MACHINE_ARCH', 'SIGGEN_LOCKEDSIGS_TYPES', '__BBTASKS', 'OSTREE_ROOTFS', 'EXTRA_OSTREE_COMMIT', 'OSTREE_COMMIT_SKIP_IF_UNCHANGED', 'SOC_FAMILY', 'KERNEL_DEVICETREE', 'UKI_DEVICETREE', 'SRC_URI', 'SD_BOOT_PATCHES', 'PACKAGECONFIG')
    values = {key: data.getVar(key) or '' for key in keys}
    values['QCOM_MACHINE_DEVICETREE'] = builder.data.getVar('KERNEL_DEVICETREE') or ''
    values['QCOM_MACHINE_SOC_FAMILY'] = builder.data.getVar('SOC_FAMILY') or ''
    print('QCOM_OTA_VARIABLES=' + json.dumps(values))


if __name__ == '__main__':
    if len(sys.argv) == 5 and sys.argv[1] == '--parse-worker':
        parse_worker(*sys.argv[2:])
    else:
        raise SystemExit(main())
