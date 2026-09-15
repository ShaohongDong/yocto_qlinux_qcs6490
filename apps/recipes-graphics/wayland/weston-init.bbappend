# SPDX-License-Identifier: MIT
# Weston desktop-shell does not enumerate XDG .desktop application entries.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append = "${@' file://imx708-camera.png' if d.getVar('QCOM_APP') == 'app002-imx708-camera' else ''}"

do_install:append() {
    if [ "${QCOM_APP}" = "app002-imx708-camera" ]; then
        install -Dm0644 ${UNPACKDIR}/imx708-camera.png ${D}${datadir}/weston/imx708-camera.png
        cat >> ${D}${sysconfdir}/xdg/weston/weston.ini <<'LAUNCHER'

[launcher]
icon=/usr/share/weston/imx708-camera.png
path=/usr/bin/imx708-camera-launch
LAUNCHER
    fi
}
FILES:${PN} += "${datadir}/weston/imx708-camera.png"

# PNG is installed by the selected application, alongside its desktop entry.
do_install:append() {
    if [ "${QCOM_APP}" = "app005-gpu-benchmark" ]; then
        cat >> ${D}${sysconfdir}/xdg/weston/weston.ini <<'GPU_LAUNCHER'

[launcher]
icon=/usr/share/weston/gpu-benchmark.png
path=/usr/bin/gpu-benchmark --gui
GPU_LAUNCHER
    fi
    if [ "${QCOM_APP}" = "app003-hevc-benchmark" ]; then
        cat >> ${D}${sysconfdir}/xdg/weston/weston.ini <<'HEVC_LAUNCHER'

[launcher]
icon=/usr/share/weston/hevc-benchmark.png
path=/usr/bin/hevc-benchmark --gui
HEVC_LAUNCHER
    fi
}
