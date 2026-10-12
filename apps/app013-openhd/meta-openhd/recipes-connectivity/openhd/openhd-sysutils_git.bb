# SPDX-License-Identifier: MIT
SUMMARY = "Pinned upstream OpenHD sysutils development baseline"
HOMEPAGE = "https://github.com/OpenHD/OpenHD-SysUtils"
LICENSE = "GPL-3.0-only & OpenHD-Non-Military"
LIC_FILES_CHKSUM = "file://systemd/openhd-sys-utils.service;beginline=1;endline=22;md5=11b8870a8a32c7a64b21e0495e940846"
PV = "1.0.3+gitdca9b1baa7c4"
SRC_URI = "git://github.com/OpenHD/OpenHD-SysUtils;protocol=https;nobranch=1;destsuffix=sysutils \
           file://0001-Use-a-reproducible-sysutils-version.patch \
           file://0002-Split-native-compiler-command-for-code-generation.patch"
SRCREV = "dca9b1baa7c4cce3adf7e5bea96dddde9644582d"
S = "${UNPACKDIR}/sysutils"
inherit cmake
export HOST_CXX = "${BUILD_CXX}"
EXTRA_OECMAKE = "-DOPENHD_X20_PACKAGE=OFF -DOPENHD_BUILD_ID=dca9b1baa7c4"

# Install only the development executable, without first-boot/startup hooks.
do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/openhd_sys_utils ${D}${bindir}/
}
RDEPENDS:${PN} += "iw iproute2 networkmanager-nmcli"
COMPATIBLE_MACHINE = "${@'^radxa-dragon-q6a$' if d.getVar('QCOM_APP') == 'app013-openhd' else '^$'}"
