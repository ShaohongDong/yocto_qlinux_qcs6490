# SPDX-License-Identifier: MIT
SUMMARY = "Q6A EFI System Partition with embloader and a BLS boot entry"
LICENSE = "MIT"
inherit image features_check
require esp-qcom-common.inc

COMPATIBLE_MACHINE = "^radxa-dragon-q6a$"
REQUIRED_MACHINE_FEATURES = "efi"
PACKAGE_INSTALL = ""
EMBLOADER_VERSION = "0.7"
EMBLOADER_CMDLINE = "root=${QCOM_BOOTIMG_ROOTFS} rw rootwait console=${KERNEL_CONSOLE} ${@d.getVar('KERNEL_CMDLINE_EXTRA') or ''}"
do_image[depends] += "embloader:do_deploy virtual/kernel:do_deploy ${INITRAMFS_IMAGE}:do_image_complete"
IMAGE_PREPROCESS_COMMAND:append = " populate_embloader_esp;"

python populate_embloader_esp() {
    import pathlib
    import shutil
    import tarfile

    deploy = pathlib.Path(d.getVar('DEPLOY_DIR_IMAGE'))
    root = pathlib.Path(d.getVar('IMAGE_ROOTFS'))
    machine = d.getVar('MACHINE')
    with tarfile.open(deploy / ('modules-%s.tgz' % machine)) as archive:
        releases = {parts[2] for member in archive
                    if len(parts := pathlib.PurePosixPath(member.name).parts) >= 3
                    and parts[:2] == ('lib', 'modules')}
    if len(releases) != 1:
        bb.fatal('Expected exactly one deployed kernel release: %s' % sorted(releases))
    release = releases.pop()
    # do_image can rerun without do_rootfs after a kernel deployment changes.
    # Remove this recipe's old payload before populating the selected release.
    for relative in ('EFI/BOOT', 'loader', 'RadxaOS'):
        previous = root / relative
        if previous.exists():
            shutil.rmtree(previous)
    boot = root / 'EFI/BOOT'
    boot.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(deploy / ('embloader-%s.efi' % d.getVar('EMBLOADER_VERSION')),
                    boot / 'BOOTAA64.EFI')
    payload = root / 'RadxaOS' / release
    payload.mkdir(parents=True, exist_ok=True)
    initramfs = '%s-%s.%s' % (d.getVar('INITRAMFS_IMAGE'), machine,
                              d.getVar('INITRAMFS_FSTYPES').split()[0])
    for source, target in [('Image', 'Image'), (initramfs, 'initramfs.cpio.gz'),
                           ('qcs6490-radxa-dragon-q6a.dtb', 'q6a.dtb')]:
        shutil.copyfile(deploy / source, payload / target)
    loader = root / 'loader'
    (loader / 'entries').mkdir(parents=True, exist_ok=True)
    (loader / 'loader.conf').write_text('timeout 3\ndefault radxa-dragon-q6a\n')
    entry = ('title Radxa Dragon Q6A\nversion %s\nlinux /RadxaOS/%s/Image\n'
             'initrd /RadxaOS/%s/initramfs.cpio.gz\n'
             'devicetree /RadxaOS/%s/q6a.dtb\noptions %s\n')
    (loader / 'entries/radxa-dragon-q6a.conf').write_text(
        entry % (release, release, release, release, d.getVar('EMBLOADER_CMDLINE')))
}
