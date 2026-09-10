SECTION = "kernel"

DESCRIPTION = "Radxa Linux ${PV} kernel for Dragon Q6A and Q8B"
HOMEPAGE = "https://github.com/radxa/kernel"
LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

inherit kernel cml1

COMPATIBLE_MACHINE = "^(radxa-dragon-q6a|radxa-dragon-q8b)$"

LINUX_QCOM_FIT_DTB_COMPATIBLE = "conf/machine/include/fit-dtb-compatible-linux-qcom.inc"

LINUX_VERSION = "7.0.11"
PV = "${LINUX_VERSION}"
PR = "r6"
KERNEL_LOCALVERSION = "-6-qcom"

RADXA_KERNEL_PACKAGING_SRCREV = "69f6ffe75b23f31312fc94b1b7f748929d41d9c9"
SRCREV = "657c0f722940cd9d3b51abfa7383655ec7d2c795"
SRCBRANCH = "linux-7.0.11"

FILESEXTRAPATHS:prepend := "${THISDIR}/linux-qcom-7.0.11:"

# The two local patches are copied from radxa-pkg/linux-qcom at
# RADXA_KERNEL_PACKAGING_SRCREV. Their original SHA-256 values are:
#   491c1d0d8abaf688566051973723c4e1e412b92a6e49781520869cefb70fed0b
#   c991856dc3f03e4ca9a14592921bcfb984986ac7675203fdfd560e295f986218
# Only the OpenEmbedded Upstream-Status metadata was added locally.
SRC_URI = " \
    git://github.com/radxa/kernel.git;branch=${SRCBRANCH};protocol=https \
    file://0001-feat-Radxa-common-kernel-config.patch;striplevel=2 \
    file://0002-feat-Radxa-custom-kernel-config.patch;striplevel=2 \
    file://0003-q6a-pmic-gpio-default.patch \
"

S = "${UNPACKDIR}/${BP}"

KBUILD_DEFCONFIG = "defconfig"

# Match the configuration order used by radxa-pkg/linux-qcom 7.0.11-6.
RADXA_KBUILD_CONFIG = " \
    ${S}/arch/arm64/configs/qcom_module.config \
    ${S}/arch/arm64/configs/radxa.config \
    ${S}/arch/arm64/configs/radxa_custom.config \
"
KBUILD_CONFIG_EXTRA = "${@bb.utils.contains('DISTRO_FEATURES', 'hardened', '${S}/kernel/configs/hardening.config', '', d)}"
KBUILD_CONFIG_EXTRA:append = " ${RADXA_KBUILD_CONFIG}"
KBUILD_CONFIG_EXTRA:append = " ${@oe.utils.vartrue('DEBUG_BUILD', '${S}/kernel/configs/debug.config', '', d)}"

KERNEL_PAHOLE ?= '${@oe.utils.vartrue("DEBUG_BUILD", bb.utils.contains("BBFILE_COLLECTIONS", "openembedded-layer", "1", "0", d), "0", d)}'
do_configure[depends] += '${@oe.utils.vartrue("KERNEL_PAHOLE", "pahole-native:do_populate_sysroot", "", d)}'
EXTRA_OEMAKE += '${@oe.utils.vartrue("KERNEL_PAHOLE", "", "PAHOLE=false", d)}'

do_configure:prepend() {
    cp ${S}/arch/${ARCH}/configs/${KBUILD_DEFCONFIG} ${B}/.config
    ${S}/scripts/kconfig/merge_config.sh -m -O ${B} \
        ${B}/.config ${KBUILD_CONFIG_EXTRA} ${@" ".join(find_cfgs(d))}
}
