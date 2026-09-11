# SPDX-License-Identifier: MIT
# Only inherited by the two explicitly supported proprietary EFI images.
# Remove this blocker when the matching CHI adapter, module/tuning artifacts
# and CAM3 platform DT are integrated; no configuration switch bypasses it.
python do_imx708_support_check() {
    if ((d.getVar("QCOM_APP") or "").strip() != "imx708-camera"
            or d.getVar("QCOM_APP_IMAGE_ACTIVE") != "1"):
        return
    bb.fatal("IMX708 CAM3 image support is incomplete: reference CHI candidates have unverified "
             "target ABI/format compatibility, and the sensor bundle, Wide tuning and full "
             "CamX platform/CAM3 DT are not integrated. Build qcom-selected-app and imx708-sensor-core independently "
             "for development. See apps/imx708-camera/docs/bringup.md.")
}
addtask imx708_support_check before do_rootfs
