SUMMARY = "Radxa Dragon Q8B SC8280XP firmware and DSP libraries"
DESCRIPTION = "Pinned boot firmware and Hexagon userspace binaries from Radxa's Q8B firmware package."
HOMEPAGE = "https://github.com/radxa-pkg/radxa-firmware"

LICENSE = "CLOSED & dspso-qcom & MIT"
LIC_FILES_CHKSUM = " \
    file://LICENSE.qcom;md5=164e3362a538eb11d3ac51e8e134294b \
    file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302 \
"
NO_GENERIC_LICENSE[dspso-qcom] = "LICENSE.qcom"

FILESEXTRAPATHS:prepend := "${LAYERDIR_qcom}/licenses:"
SRC_URI = " \
    git://github.com/radxa-pkg/radxa-firmware.git;branch=main;protocol=https \
    file://LICENSE.qcom;subdir=${BP} \
"
SRCREV = "e1761009df008adfd62c77f2c5584e3067449013"

S = "${UNPACKDIR}/${BP}"

inherit allarch

INHIBIT_PACKAGE_DEBUG_SPLIT = "1"
INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_DEFAULT_DEPS = "1"

do_configure[noexec] = "1"
do_compile[noexec] = "1"

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware ${D}${datadir}/qcom
    cp -R --no-dereference --preserve=mode,timestamps \
        ${S}/radxa-firmware-sc8280xp/lib/firmware/. \
        ${D}${nonarch_base_libdir}/firmware/
    cp -R --no-dereference --preserve=mode,timestamps \
        ${S}/radxa-firmware-sc8280xp/usr/share/qcom/. \
        ${D}${datadir}/qcom/

    install -d ${D}${datadir}/qcom/conf.d
    printf '%s\n' \
        '# SPDX-License-Identifier: MIT' \
        'machines:' \
        '  Radxa Dragon Q8B:' \
        '    DSP_LIBRARY_PATH: sc8280xp/radxa/dragon-q8b/dsp' \
        > ${D}${datadir}/qcom/conf.d/hexagon-dsp-binaries-radxa-dragon-q8b.yaml
}

PACKAGES = " \
    ${PN}-firmware \
    ${PN}-adsp \
    ${PN}-cdsp \
    ${PN}-config \
    ${PN} \
"

LICENSE:${PN}-firmware = "CLOSED"
LICENSE:${PN}-adsp = "dspso-qcom"
LICENSE:${PN}-cdsp = "dspso-qcom"
LICENSE:${PN}-config = "MIT"

FILES:${PN}-firmware = "${nonarch_base_libdir}/firmware/qcom"
FILES:${PN}-adsp = "${datadir}/qcom/sc8280xp/radxa/dragon-q8b/dsp/adsp"
FILES:${PN}-cdsp = "${datadir}/qcom/sc8280xp/radxa/dragon-q8b/dsp/cdsp"
FILES:${PN}-config = "${datadir}/qcom/conf.d/hexagon-dsp-binaries-radxa-dragon-q8b.yaml"
FILES:${PN} = ""
ALLOW_EMPTY:${PN} = "1"

RDEPENDS:${PN}-adsp = "${PN}-config"
RDEPENDS:${PN}-cdsp = "${PN}-config"

INSANE_SKIP:${PN}-adsp = "arch libdir file-rdeps textrel"
INSANE_SKIP:${PN}-cdsp = "arch libdir file-rdeps textrel"
INSANE_SKIP:${PN}-firmware = "arch already-stripped"
SKIP_FILEDEPS:${PN}-adsp = "1"
SKIP_FILEDEPS:${PN}-cdsp = "1"
