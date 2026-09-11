# SPDX-License-Identifier: MIT
# Boot hooks differ between fixed Android and EFI deployments.
allarch_package_arch_handler:prepend() {
    if d.getVar('QCOM_OTA_ENABLE') == '1':
        d.setVar('PACKAGE_ARCH', d.getVar('MACHINE_ARCH'))
        return
}
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = "${@' file://qcom-ota-fixed' if d.getVar('QCOM_OTA_ENABLE') == '1' else ''}"
do_install:append() {
    if [ "${QCOM_OTA_ENABLE}" = "1" ] && [ "${QCOM_OTA_BOOT_BACKEND}" = "fixed" ]; then
        install -m 0755 ${UNPACKDIR}/qcom-ota-fixed ${D}/init.d/98-ostree
    fi
}
