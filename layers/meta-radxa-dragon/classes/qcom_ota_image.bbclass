# SPDX-License-Identifier: MIT
QCOM_OTA_IMAGE = "${@'1' if (d.getVar('PN') or '').startswith('qcom-') and (d.getVar('PN') or '').endswith('-image') else '0'}"
# Apply before image.bbclass creates tasks, not in a later anonymous function.
IMAGE_FSTYPES:remove = "${@'ext4 ext4.gz ext4.xz ext4.zst' if d.getVar('QCOM_OTA_IMAGE') == '1' else ''}"
do_image_ostree[postfuncs] += "qcom_ota_identity"
do_image_ostreecommit[postfuncs] += "qcom_ota_manifest"
do_image_ota[postfuncs] += "qcom_ota_boot_setup"
do_image_ota[depends] += "${@'embloader:do_deploy' if d.getVar('QCOM_OTA_BOOT_BACKEND') == 'embloader' else ''}"
do_image_wic[depends] += "${@'%s:do_image_ota_esp %s:do_image_ota_ext4' % (d.getVar('PN'), d.getVar('PN')) if 'wic' in (d.getVar('IMAGE_FSTYPES') or '').split() else ''}"
WICVARS:append = " IMAGE_LINK_NAME"

python __anonymous() {
    if d.getVar('QCOM_OTA_IMAGE') != '1':
        for var in ('IMAGE_FSTYPES', 'IMAGE_CLASSES'):
            excluded = {'ota-esp', 'ota-ext4', 'ostreecommit.tar.xz', 'qcomflash', 'uki'}
            d.setVar(var, ' '.join(x for x in (d.getVar(var) or '').split() if x not in excluded))
        d.setVar('IMAGE_INSTALL:remove', ' '.join((d.getVar('SOTA_CLIENT_PACKAGES') or '').split() + ['ostree', 'ostree-kernel', 'ostree-initramfs', 'ostree-devicetrees']))
        return
    import re
    for var in ('QCOM_OTA_CHANNEL', 'MACHINE', 'IMAGE_BASENAME'):
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]*', d.getVar(var) or ''):
            bb.fatal('%s must be a single safe ref component' % var)
    if not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', d.getVar('QCOM_OTA_VERSION') or ''):
        bb.fatal('QCOM_OTA_VERSION must be MAJOR.MINOR.PATCH')
}

qcom_ota_identity[vardeps] += "SERIAL_CONSOLES KERNEL_CMDLINE_EXTRA QCOM_BOOTIMG_ROOTFS SD_QCOM_BOOTIMG_ROOTFS QCOM_BOOTIMG_PAGE_SIZE QCOM_BOOTIMG_KERNEL_BASE"

python qcom_ota_identity() {
    import hashlib
    import json
    import pathlib
    root = pathlib.Path(d.getVar('OSTREE_ROOTFS'))
    modules = root / 'usr/lib/modules'
    # Hash paths and contents, including modules and the fixed boot payload.
    digest = hashlib.sha256()
    for path in sorted(modules.rglob('*')):
        digest.update(str(path.relative_to(modules)).encode() + b'\0')
        if path.is_symlink():
            digest.update(path.readlink().as_posix().encode())
        elif path.is_file():
            with path.open('rb') as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b''):
                    digest.update(block)
    if d.getVar('QCOM_OTA_BOOT_BACKEND') == 'fixed':
        # Android boot images also append DTBs and bake in per-board boot
        # parameters. A userspace-only update must not silently change those.
        deploy = pathlib.Path(d.getVar('DEPLOY_DIR_IMAGE')) / (d.getVar('KERNEL_DEPLOYSUBDIR') or '')
        for name in sorted((d.getVar('KERNEL_DEVICETREE') or '').split()):
            dtb = deploy / pathlib.Path(name).name
            digest.update(name.encode() + b'\0')
            digest.update(dtb.read_bytes())
            board = pathlib.Path(name).stem
            for variable in ('SERIAL_CONSOLES', 'KERNEL_CMDLINE_EXTRA', 'QCOM_BOOTIMG_ROOTFS',
                             'SD_QCOM_BOOTIMG_ROOTFS', 'QCOM_BOOTIMG_PAGE_SIZE', 'QCOM_BOOTIMG_KERNEL_BASE'):
                value = d.getVarFlag(variable, board)
                if value is None:
                    value = d.getVar(variable)
                digest.update(variable.encode() + b'\0' + (value or '').encode() + b'\0')
    identity = dict(schema=1, machine=d.getVar('MACHINE'), image=d.getVar('IMAGE_BASENAME'),
                    channel=d.getVar('QCOM_OTA_CHANNEL'), ref=d.getVar('OSTREE_BRANCHNAME'),
                    version=d.getVar('QCOM_OTA_VERSION'), backend=d.getVar('QCOM_OTA_BOOT_BACKEND'),
                    kernel_fingerprint=digest.hexdigest(), min_free_kib=int(d.getVar('QCOM_OTA_MIN_FREE_KIB')))
    target = root / 'usr/share/qcom-ota/identity.json'
    target.parent.mkdir(parents=True, exist_ok=True)
    fstab = root / 'usr/etc/fstab'
    lines = fstab.read_text().splitlines() if fstab.exists() else []
    lines = [line for line in lines if len(line.split()) < 2 or line.split()[1] not in ('/boot', '/boot/efi', '/')]
    if identity['backend'] != 'fixed':
        lines.append('LABEL=otaboot /boot vfat defaults 0 2')
    fstab.write_text('\n'.join(lines) + '\n')
    identity['installed_bytes'] = sum(p.stat().st_size for p in root.rglob('*') if p.is_file() and not p.is_symlink())
    # OSTree uses the UKI instead of the raw vmlinuz/initramfs when present.
    # vmlinuz and uki.efi are hard links; counting both can reject an update
    # even when its boot payload and rollback fit in the ESP.
    identity['boot_bytes'] = 0
    for kernel in modules.iterdir():
        if not kernel.is_dir():
            continue
        names = ('uki.efi',) if (kernel / 'uki.efi').is_file() else ('vmlinuz', 'initramfs.img', 'devicetree')
        identity['boot_bytes'] += sum((kernel / name).stat().st_size for name in names if (kernel / name).is_file())
    target.write_text(json.dumps(identity, sort_keys=True, indent=2) + '\n')
}

EXTRA_OSTREE_COMMIT:append = ' --add-metadata-string=qcom.ota="$(cat ${OSTREE_ROOTFS}/usr/share/qcom-ota/identity.json)"'
# OSTree's skip check compares the file tree, not the supplied metadata. A
# metadata correction must not silently reuse an older, incompatible commit.
OSTREE_COMMIT_SKIP_IF_UNCHANGED = "0"

python qcom_ota_manifest() {
    import ast
    import json
    import pathlib
    import subprocess
    identity = json.loads((pathlib.Path(d.getVar('OSTREE_ROOTFS')) / 'usr/share/qcom-ota/identity.json').read_text())
    checksum = (pathlib.Path(d.getVar('WORKDIR')) / 'ostree_manifest').read_text().strip()
    encoded = subprocess.check_output(['ostree', '--repo=' + d.getVar('OSTREE_REPO'),
                                       'show', '--print-metadata-key=qcom.ota', checksum], text=True)
    try:
        metadata = json.loads(ast.literal_eval(encoded))
    except (ValueError, SyntaxError):
        bb.fatal('OSTree commit has malformed OTA metadata')
    if metadata != identity:
        bb.fatal('OSTree commit metadata differs from the OTA filesystem identity')
    identity['commit'] = checksum
    deploy = pathlib.Path(d.getVar('IMGDEPLOYDIR'))
    (deploy / (d.getVar('IMAGE_LINK_NAME') + '.ota.json')).write_text(json.dumps(identity, indent=2) + '\n')
}

python qcom_ota_boot_setup() {
    import pathlib
    import shutil
    import subprocess
    root = pathlib.Path(d.getVar('OTA_SYSROOT'))
    backend = d.getVar('QCOM_OTA_BOOT_BACKEND')
    boot = pathlib.Path(d.getVar('OTA_BOOT')) / 'boot'
    # Seed packaged /var data on the initial installation only.
    source_var = pathlib.Path(d.getVar('IMAGE_ROOTFS')) / 'var'
    # OSTree already creates some /var links. cp -a merges those links and
    # preserves packaged ownership; copytree fails on existing symlinks.
    subprocess.run(['cp', '-a', str(source_var) + '/.',
                    str(root / 'ostree/deploy/qcom/var')], check=True)
    if backend == 'fixed':
        deployments = list((root / 'ostree/deploy/qcom/deploy').glob('*.0'))
        if len(deployments) != 1:
            bb.fatal('Expected one factory OSTree deployment')
        (root / 'ostree/qcom-active').symlink_to(deployments[0].relative_to(root / 'ostree'))
        return
    if backend == 'embloader':
        loader = boot / d.getVar('EFIDIR').lstrip('/') / d.getVar('EFI_BOOT_IMAGE')
        shutil.copyfile(pathlib.Path(d.getVar('DEPLOY_DIR_IMAGE')) / 'embloader-0.7.efi', loader)
    (boot / 'loader/loader.conf').write_text('timeout 3\ndefault ostree-*\n')
    # The factory installation is the initial known-good fallback.
    import re
    for entry in (boot / 'loader/entries').glob('*.conf'):
        entry.rename(entry.with_name(re.sub(r'\+[0-9]+(?:-[0-9]+)?(?=\.conf$)', '', entry.name)))
    # Never depend on a stale ESP UUID from the separate non-OTA ESP recipe.
    for deployment in (root / 'ostree/deploy/qcom/deploy').glob('*.0'):
        fstab = deployment / 'etc/fstab'
        lines = fstab.read_text().splitlines() if fstab.exists() else []
        lines = [line for line in lines if len(line.split()) < 2 or line.split()[1] not in ('/boot', '/boot/efi', '/')]
        lines.append('LABEL=otaboot /boot vfat defaults 0 2')
        fstab.write_text('\n'.join(lines) + '\n')
}

# The WIC/rawcopy images retain the FAT label generated here.
EXTRA_IMAGECMD:ota-esp:append = " -n otaboot"

python set_image_size:append() {
        if d.getVar('BB_CURRENTTASK') == 'image_ota_ext4' and d.getVar('QCOM_OTA_IMAGE') == '1':
            import json
            import pathlib
            import subprocess
            identity = json.loads((pathlib.Path(d.getVar('OSTREE_ROOTFS')) / 'usr/share/qcom-ota/identity.json').read_text())
            used = int(subprocess.check_output(['du', '-sk', d.getVar('OTA_SYSROOT')], text=True).split()[0])
            # Space for a complete download and checkout, even with no shared
            # objects, plus filesystem metadata and the runtime reserve.
            needed = used + (2 * identity['installed_bytes'] + 1023) // 1024 + identity['min_free_kib']
            # Dense inode tables (-i 4096), ext4's reserved blocks and journal
            # are unavailable to the OTA client. Allow for all three, not just
            # the file contents reported by du.
            needed = ((needed * 5 // 4 + 131072 + 4095) // 4096) * 4096
            needed = max(needed, int(d.getVar('ROOTFS_SIZE')))
            maximum = d.getVar('IMAGE_ROOTFS_MAXSIZE')
            if maximum and needed > int(maximum):
                bb.fatal('OTA rootfs including update reserve exceeds IMAGE_ROOTFS_MAXSIZE')
            d.setVar('ROOTFS_SIZE', str(needed))
}

# The legacy packager otherwise falls back to boot-${MACHINE}.img, which
# contains no initramfs and therefore cannot enter an OSTree deployment.
create_qcomflash_pkg:prepend() {
    if [ "${QCOM_OTA_BOOT_BACKEND}" = "fixed" ]; then
        install -d boot-images
        ota_first_dtb=""
        for ota_tree in ${KERNEL_DEVICETREE}; do
            ota_dtb=$(basename "$ota_tree" .dtb)
            ota_boot="${DEPLOY_DIR_IMAGE}/boot-initramfs-$ota_dtb-${MACHINE}.img"
            test -s "$ota_boot" || bbfatal "Missing fixed OTA boot image: $ota_boot"
            install -m 0644 "$ota_boot" "boot-images/$ota_dtb.img"
            test -n "$ota_first_dtb" || ota_first_dtb="$ota_dtb"
        done
        ota_default_dtb="${QCOM_DTB_DEFAULT}"
        if [ -z "$ota_default_dtb" ] || [ "$ota_default_dtb" = "multi-dtb" ]; then
            ota_default_dtb="$ota_first_dtb"
        fi
        test -n "$ota_default_dtb" && test -s "boot-images/$ota_default_dtb.img" || \
            bbfatal "Select a supported fixed OTA boot image with QCOM_DTB_DEFAULT"
        install -m 0644 "boot-images/$ota_default_dtb.img" boot.img
        printf '%s\n' "$ota_default_dtb" > boot-images/default-dtb
    fi
}
