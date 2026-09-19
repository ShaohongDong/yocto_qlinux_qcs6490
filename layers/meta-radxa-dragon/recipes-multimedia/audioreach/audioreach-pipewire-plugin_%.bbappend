# SPDX-License-Identifier: MIT
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append:radxa-dragon-q6a = " file://0001-pal-headphone-only-jack-policy.patch file://q6a-pw-pal-plugin.conf"
PACKAGE_ARCH:radxa-dragon-q6a = "${MACHINE_ARCH}"
do_install:append:radxa-dragon-q6a() {
    install -m 0644 ${UNPACKDIR}/q6a-pw-pal-plugin.conf \
        ${D}${datadir}/pipewire/pipewire.conf.d/pw-pal-plugin.conf
    # The generic policy redirects headphones to a nonexistent speaker.
    printf '# Q6A headphone-only routing is handled by the PAL plugin.\n' > \
        ${D}${datadir}/wireplumber/wireplumber.conf.d/90-device-detection.conf
}
