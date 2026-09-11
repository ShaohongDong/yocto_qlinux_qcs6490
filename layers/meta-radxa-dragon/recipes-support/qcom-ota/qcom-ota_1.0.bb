# SPDX-License-Identifier: MIT
SUMMARY = "Manual signed OSTree updates for Qualcomm boards"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
SRC_URI = "file://qcom_ota.py file://qcom-ota-confirm.service file://qcom-ota-health.conf"
S = "${UNPACKDIR}"
inherit allarch systemd
RDEPENDS:${PN} = "python3-core python3-json python3-io ostree util-linux-mountpoint"
SYSTEMD_SERVICE:${PN} = "qcom-ota-confirm.service"
SYSTEMD_AUTO_ENABLE = "enable"
do_install() {
    install -Dm0755 ${UNPACKDIR}/qcom_ota.py ${D}${bindir}/qcom-ota
    install -Dm0644 ${UNPACKDIR}/qcom-ota-confirm.service ${D}${systemd_system_unitdir}/qcom-ota-confirm.service
    install -Dm0644 ${UNPACKDIR}/qcom-ota-health.conf ${D}${sysconfdir}/qcom-ota/health.json
    # Only our health-checked service may bless a pending boot.
    install -d ${D}${systemd_system_unitdir}/systemd-bless-boot.service.d
    printf '[Unit]\nConditionPathExists=/run/qcom-ota-health-passed\n' \
        > ${D}${systemd_system_unitdir}/systemd-bless-boot.service.d/qcom-ota.conf
}
FILES:${PN} += "${systemd_system_unitdir}/systemd-bless-boot.service.d/qcom-ota.conf"
CONFFILES:${PN} += "${sysconfdir}/qcom-ota/health.json"
