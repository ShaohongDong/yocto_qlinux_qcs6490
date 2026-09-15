# SPDX-License-Identifier: MIT
# This client discovers GTK, GStreamer and camera-service through pkg-config.
# Preserve the existing reference application's build environment.
inherit_defer ${@'pkgconfig' if d.getVar('QCOM_APP') in ('imx708-camera', 'hevc-benchmark') else ''}

FILES:${PN}:append = "${@' ${datadir}/weston/hevc-benchmark.png ${datadir}/icons/hicolor/scalable/apps/hevc-benchmark.svg' if d.getVar('QCOM_APP') == 'hevc-benchmark' else ''}"
