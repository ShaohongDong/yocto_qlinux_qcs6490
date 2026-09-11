# SPDX-License-Identifier: MIT
# All Qualcomm production images are OTA images unless explicitly disabled.
QCOM_OTA_ENABLE ??= "1"
QCOM_OTA_CHANNEL ??= "stable"
QCOM_OTA_VERSION ??= "${DISTRO_VERSION}.0"
QCOM_OTA_BOOT_BACKEND ??= "${@'systemd-boot' if bb.utils.contains('MACHINE_FEATURES', 'efi', True, False, d) else 'fixed'}"
QCOM_OTA_BOOT_BACKEND:radxa-dragon-q6a = "embloader"
QCOM_OTA_BOOT_BACKEND:qcom-armv7a = "fixed"
QCOM_OTA_MIN_FREE_KIB ??= "524288"

DISTRO_FEATURES:append = "${@' sota usrmerge' if d.getVar('QCOM_OTA_ENABLE') == '1' else ''}"
DISTRO_FEATURES_NATIVE:append = "${@' sota' if d.getVar('QCOM_OTA_ENABLE') == '1' else ''}"
DISTRO_FEATURES_OPTED_OUT:append = "${@' ptest' if d.getVar('QCOM_OTA_ENABLE') == '1' else ''}"
INHERIT += "${@'qcom_ota' if d.getVar('QCOM_OTA_ENABLE') == '1' else ''}"
INITRAMFS_IMAGE = "${@'initramfs-ostree-image' if d.getVar('QCOM_OTA_ENABLE') == '1' else 'initramfs-rootfs-image'}"
