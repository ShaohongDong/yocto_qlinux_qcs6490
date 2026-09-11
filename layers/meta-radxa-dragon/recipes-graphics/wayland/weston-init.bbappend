# SPDX-License-Identifier: MIT
FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

DEFAULTBACKEND:qcom ?= "drm"

SRC_URI:append:qcom = " \
    file://additional-devices.conf \
    file://weston-start.sh \
"

do_install:append:qcom() {
    install -d ${D}${systemd_system_unitdir}/weston.service.d
    install -m 0644 ${UNPACKDIR}/additional-devices.conf \
        ${D}${systemd_system_unitdir}/weston.service.d/additional-devices.conf
    sed -i -e 's:@bindir@:${bindir}:g' \
        ${D}${systemd_system_unitdir}/weston.service.d/additional-devices.conf

    install -d ${D}${bindir}
    install -m 0755 ${UNPACKDIR}/weston-start.sh \
        ${D}${bindir}/weston-start.sh
    sed -i -e 's:@bindir@:${bindir}:g' \
        ${D}${bindir}/weston-start.sh
}

FILES:${PN} += "${systemd_system_unitdir}/weston.service.d/additional-devices.conf"
FILES:${PN} += "${bindir}/weston-start.sh"

do_install:append:radxa-dragon-q6a() {
    # The tested HDMI sink corrupts 10-bpc output although its EDID advertises
    # deep color. Keep GPU rendering and EDID mode selection, but use 8 bpc.
    cat >> ${D}${sysconfdir}/xdg/weston/weston.ini <<'EOF'

[output]
name=HDMI-A-1
max-bpc=8
EOF
}
