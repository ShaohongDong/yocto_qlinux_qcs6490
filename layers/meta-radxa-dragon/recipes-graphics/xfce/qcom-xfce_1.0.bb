# SPDX-License-Identifier: MIT
SUMMARY = "Q6A XFCE autologin session and OTA desktop health check"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"
COMPATIBLE_MACHINE = "^radxa-dragon-q6a$"

SRC_URI = "file://qcom-xfce-start file://qcom-xfce-session file://qcom-xfce-ready \
           file://xfce.conf file://qcom-xfce-ready.service file://qcom-xfce-autologin \
           file://20-qcom-xfce.conf file://qcom-xfce-graphics-check.c"
S = "${UNPACKDIR}"

inherit systemd features_check
REQUIRED_DISTRO_FEATURES = "x11 pam systemd opengl"
DEPENDS = "virtual/libgl libx11"
RDEPENDS:${PN} = "packagegroup-xfce-base xserver-nodm-init xserver-xorg \
                 xf86-input-libinput xf86-video-modesetting weston-init dbus xauth xprop xrandr \
                 util-linux-mcookie procps mesa-demos-info mesa-megadriver coreutils q6a-audio-access"
SYSTEMD_SERVICE:${PN} = "qcom-xfce-ready.service"

do_compile() {
    ${CC} ${CFLAGS} ${CPPFLAGS} ${S}/qcom-xfce-graphics-check.c \
        -o ${B}/qcom-xfce-graphics-check ${LDFLAGS} -lGL -lX11
}

do_install() {
    install -Dm0755 ${B}/qcom-xfce-graphics-check ${D}${bindir}/qcom-xfce-graphics-check
    for script in qcom-xfce-start qcom-xfce-session qcom-xfce-ready; do
        install -Dm0755 ${S}/$script ${D}${bindir}/$script
    done
    install -Dm0644 ${S}/xfce.conf ${D}${systemd_system_unitdir}/xserver-nodm.service.d/xfce.conf
    install -Dm0644 ${S}/qcom-xfce-ready.service ${D}${systemd_system_unitdir}/qcom-xfce-ready.service
    install -Dm0644 ${S}/qcom-xfce-autologin ${D}${sysconfdir}/pam.d/qcom-xfce-autologin
    install -Dm0644 ${S}/20-qcom-xfce.conf ${D}${sysconfdir}/X11/xorg.conf.d/20-qcom-xfce.conf
    install -d ${D}${sysconfdir}/systemd/system/graphical.target.wants
    ln -s ${systemd_system_unitdir}/xserver-nodm.service \
        ${D}${sysconfdir}/systemd/system/graphical.target.wants/xserver-nodm.service
}

FILES:${PN} += "${systemd_system_unitdir}/xserver-nodm.service.d ${sysconfdir}/systemd/system"
CONFFILES:${PN} += "${sysconfdir}/X11/xorg.conf.d/20-qcom-xfce.conf"
