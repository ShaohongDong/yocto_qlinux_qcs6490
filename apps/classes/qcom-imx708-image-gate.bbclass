# SPDX-License-Identifier: MIT
# Only inherited by the two explicitly supported proprietary EFI images.
# Development images must explicitly opt in until sensor integration is complete.
QCOM_IMX708_ALLOW_INCOMPLETE ??= "0"
QCOM_IMX708_INCOMPLETE_REASON = "Reference CHI candidates have unverified target ABI/format compatibility; sensor bundle, Wide tuning and full CamX platform/CAM3 DT are not integrated."

python do_imx708_support_check() {
    if ((d.getVar("QCOM_APP") or "").strip() != "imx708-camera"
            or d.getVar("QCOM_APP_IMAGE_ACTIVE") != "1"):
        return
    reason = d.getVar("QCOM_IMX708_INCOMPLETE_REASON")
    if (d.getVar("QCOM_IMX708_ALLOW_INCOMPLETE") == "1"
            and d.getVar("MACHINE") == "radxa-dragon-q6a"):
        bb.warn("IMX708 development image: real camera capture is not validated. " + reason)
        return
    bb.fatal("IMX708 CAM3 image support is incomplete: " + reason +
             " Build components independently or explicitly select a development image "
             "with qcom-app image --allow-incomplete-camera. See apps/imx708-camera/docs/bringup.md.")
}
addtask imx708_support_check before do_rootfs

python imx708_development_marker() {
    import pathlib
    if ((d.getVar("QCOM_APP") or "").strip() == "imx708-camera"
            and d.getVar("QCOM_APP_IMAGE_ACTIVE") == "1"
            and d.getVar("MACHINE") == "radxa-dragon-q6a"
            and d.getVar("QCOM_IMX708_ALLOW_INCOMPLETE") == "1"):
        marker = pathlib.Path(d.getVar("IMAGE_ROOTFS")) / "etc/imx708-camera-development"
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text("IMX708 development image; real camera capture is not validated.\n"
                          + d.getVar("QCOM_IMX708_INCOMPLETE_REASON") + "\n", encoding="utf-8")
}
ROOTFS_POSTPROCESS_COMMAND += "imx708_development_marker; "
