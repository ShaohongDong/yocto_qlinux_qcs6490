# SPDX-License-Identifier: MIT
SUMMARY = "Q6A AudioReach access for the active desktop seat"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
COMPATIBLE_MACHINE = "^radxa-dragon-q6a$"
SRC_URI = "file://70-q6a-audioreach.rules"
S = "${UNPACKDIR}"
PACKAGE_ARCH = "${MACHINE_ARCH}"
do_install() {
    install -Dm0644 ${S}/70-q6a-audioreach.rules ${D}${nonarch_base_libdir}/udev/rules.d/70-q6a-audioreach.rules
}
RDEPENDS:${PN} = "pipewire pipewire-pulse wireplumber audioreach-conf audioreach-graphmgr audioreach-pal audioreach-pipewire-plugin"
