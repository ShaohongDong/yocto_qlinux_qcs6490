# SPDX-License-Identifier: MIT
# Only inherited by the two explicitly supported proprietary EFI images.
# Native integration is checked separately from incomplete CamX integration.
QCOM_IMX708_BACKEND ??= "native"
QCOM_IMX708_ALLOW_INCOMPLETE ??= "0"
QCOM_IMX708_INCOMPLETE_REASON = "Reference CHI candidates have unverified target ABI/format compatibility; sensor bundle, Wide tuning and full CamX platform/CAM3 DT are not integrated."

python do_imx708_support_check() {
    if ((d.getVar("QCOM_APP") or "").strip() != "imx708-camera"
            or d.getVar("QCOM_APP_IMAGE_ACTIVE") != "1"):
        return
    backend = d.getVar("QCOM_IMX708_BACKEND")
    if backend == "native" and d.getVar("MACHINE") == "radxa-dragon-q6a":
        from qcom_apps.manifest import load_manifest
        from qcom_apps.native_camera import validate_manifest
        try:
            validate_manifest(load_manifest(d.getVar("QCOM_APPS_DIR"), "imx708-camera"))
        except ValueError as error:
            bb.fatal(str(error))
        return
    if backend not in ("native", "camx"):
        bb.fatal("Unsupported IMX708 image backend: " + str(backend))
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
            and d.getVar("QCOM_IMX708_ALLOW_INCOMPLETE") == "1"
            and d.getVar("QCOM_IMX708_BACKEND") == "camx"):
        marker = pathlib.Path(d.getVar("IMAGE_ROOTFS")) / "etc/imx708-camera-development"
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text("IMX708 development image; real camera capture is not validated.\n"
                          + d.getVar("QCOM_IMX708_INCOMPLETE_REASON") + "\n", encoding="utf-8")
}
ROOTFS_POSTPROCESS_COMMAND += "imx708_development_marker; "

# Expand selection after anonymous image configuration has completed.
DEPENDS:append = "${@' dtc-native' if d.getVar('QCOM_APP') == 'imx708-camera' and d.getVar('QCOM_IMX708_BACKEND') == 'native' else ''}"

python do_imx708_native_image_check() {
    if (d.getVar("QCOM_APP") != "imx708-camera" or d.getVar("QCOM_APP_IMAGE_ACTIVE") != "1"
            or d.getVar("QCOM_IMX708_BACKEND") != "native"):
        return
    from pathlib import Path
    from qcom_apps.native_camera import audit_dtb, audit_rootfs
    try:
        audit_dtb(Path(d.getVar("DEPLOY_DIR_IMAGE")) / "qcs6490-radxa-dragon-q6a.dtb",
                  Path(d.getVar("STAGING_BINDIR_NATIVE")) / "fdtget")
        audit_rootfs(d.getVar("IMAGE_ROOTFS"))
        bb.note("Native IMX708 DTB, modules, dependencies and startup services verified")
    except Exception as error:
        bb.fatal("IMX708 native image audit failed: " + str(error))
}

# Imported Python helpers must invalidate cached image checks when edited.
do_imx708_support_check[file-checksums] += "${QCOM_APPS_DIR}/lib/qcom_apps/native_camera.py:True"
do_imx708_native_image_check[file-checksums] += "${QCOM_APPS_DIR}/lib/qcom_apps/native_camera.py:True"
do_imx708_native_image_check[depends] += "virtual/kernel:do_deploy"
addtask imx708_native_image_check after do_rootfs before do_image
