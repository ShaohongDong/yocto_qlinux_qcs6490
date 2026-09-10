# SPDX-License-Identifier: MIT

# The helper registers selected application inputs as parse-cache dependencies.
python __anonymous() {
    if (d.getVar("QCOM_APP") or "").strip() and bb.data.inherits_class("image", d):
        from qcom_apps.bitbake import image_selection
        image_selection(d)
}

qcom_app_run_rootfs_check() {
    if [ "${QCOM_APP_IMAGE_ACTIVE}" = "1" ] && [ -n "${QCOM_APP_CHECK_EXECUTABLE}" ]; then
        ${@oe.qemu.qemu_run_binary(d, '${IMAGE_ROOTFS}', '${QCOM_APP_CHECK_EXECUTABLE}')} \
            ${QCOM_APP_CHECK_ARGUMENTS}
    fi
}
