# SPDX-License-Identifier: MIT
# This client discovers GTK, GStreamer and camera-service through pkg-config.
# Preserve the existing reference application's build environment.
inherit_defer ${@'pkgconfig' if d.getVar('QCOM_APP') == 'imx708-camera' else ''}
