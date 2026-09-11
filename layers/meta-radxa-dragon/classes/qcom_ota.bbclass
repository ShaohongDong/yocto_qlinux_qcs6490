# SPDX-License-Identifier: MIT
# Common metadata; only qcom_ota_image modifies production image contents.
inherit sota

# OTA changes distro-wide filesystem and boot metadata. Frozen eSDK hashes
# can otherwise mix old packages with new metadata (even ignoring -f).
# Retain normal content-addressed sstate reuse under the current signatures.
SIGGEN_LOCKEDSIGS_TYPES:sota = ""

HOSTTOOLS += "sync"
SOTA_CLIENT_PACKAGES = "qcom-ota"
SOTA_CLIENT_PROV = ""
SOTA_DEPLOY_CREDENTIALS = "0"
BUILD_OSTREE_TARBALL = "0"
BUILD_OSTREE_REPO_TARBALL = "1"
OSTREE_OSNAME = "qcom"
OSTREE_VERSIONED_DIRS = "opt"
OSTREE_BRANCHNAME = "${MACHINE}/${@d.getVar('IMAGE_BASENAME') if d.getVar('PN') else 'image'}/${QCOM_OTA_CHANNEL}"
OSTREE_COMMIT_VERSION = "${QCOM_OTA_VERSION}"
OSTREE_BOOTLOADER = "${@'none' if d.getVar('QCOM_OTA_BOOT_BACKEND') == 'fixed' else 'systemd-boot'}"
OSTREE_KERNEL_ARGS = "root=LABEL=otaroot rootfstype=ext4 rw rootwait console=${KERNEL_CONSOLE} ${KERNEL_CMDLINE_EXTRA}"
QCOM_BOOTIMG_ROOTFS = "LABEL=otaroot"
UKI_IMAGE_CLASS = "${@'uki' if d.getVar('QCOM_OTA_BOOT_BACKEND') == 'systemd-boot' else ''}"
UKI_IMAGE_CLASS:qcom-armv7a = ""
UKI_FILENAME = "${IMAGE_BASENAME}-${MACHINE}.efi"
UKI_KERNEL_FILENAME = "Image"
# Generic multi-board machines use the firmware's DTB. Dedicated machines
# with a single DTB package that exact kernel-built tree into their UKI.
UKI_DEVICETREE = "${@d.getVar('KERNEL_DEVICETREE') if len((d.getVar('KERNEL_DEVICETREE') or '').split()) == 1 else ''}"
OSTREE_DEPLOY_DEVICETREE = "${@'1' if d.getVar('QCOM_OTA_BOOT_BACKEND') == 'embloader' else '0'}"
OSTREE_KERNEL = "${@'Image' if d.getVar('QCOM_OTA_BOOT_BACKEND') == 'embloader' else d.getVar('KERNEL_IMAGETYPE')}"

# Signing/publication is an explicit host operation, never a build side effect.
IMAGE_FSTYPES:remove = "ostreepush garagesign garagecheck"
IMAGE_CLASSES += "qcom_ota_image"

# Non-EFI devices have no ESP. Their installed Android boot image remains
# fixed and enters the active OSTree deployment through its initramfs.
IMAGE_FSTYPES:remove:qcom-armv7a = "ota-esp"
IMAGE_TYPEDEP:qcomflash:remove:qcom-armv7a = "ota-esp"
QCOM_ESP_FILE:qcom-armv7a = ""
OSTREE_REPO_CONFIG:remove:qcom-armv7a = "sysroot.boot-counting-tries:3"
OSTREE_OTA_REPO_CONFIG:remove:qcom-armv7a = "sysroot.boot-counting-tries:3"
