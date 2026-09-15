# SPDX-License-Identifier: MIT
# This client discovers GTK, GStreamer and camera-service through pkg-config.
# Preserve the existing reference application's build environment.
inherit_defer ${@'pkgconfig' if d.getVar('QCOM_APP') in ('app002-imx708-camera', 'app003-hevc-benchmark', 'app004-ai-demo', 'app005-gpu-benchmark', 'app006-wifi-test') else ''}

FILES:${PN}:append = "${@' ${datadir}/weston/hevc-benchmark.png ${datadir}/icons/hicolor/scalable/apps/hevc-benchmark.svg' if d.getVar('QCOM_APP') == 'app003-hevc-benchmark' else ''}"
FILES:${PN}:append = "${@' ${datadir}/icons/hicolor/scalable/apps/ai-demo.svg' if d.getVar('QCOM_APP') == 'app004-ai-demo' else ''}"
FILES:${PN}:append = "${@' ${datadir}/weston/gpu-benchmark.png ${datadir}/icons/hicolor/scalable/apps/gpu-benchmark.svg ${datadir}/gpu-benchmark' if d.getVar('QCOM_APP') == 'app005-gpu-benchmark' else ''}"
FILES:${PN}:append = "${@' ${datadir}/weston/wifi-test.png ${datadir}/icons/hicolor/scalable/apps/wifi-test.svg' if d.getVar('QCOM_APP') == 'app006-wifi-test' else ''}"
