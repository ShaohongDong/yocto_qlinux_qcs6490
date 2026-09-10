DESCRIPTION = "EFI System Partition Image to boot Qualcomm boards"

PACKAGE_INSTALL = " \
    systemd-boot \
"

inherit image uki uki-esp-image features_check

require esp-qcom-common.inc

UKI_FILENAME = "${EFI_LINUX_IMG}"

UKI_CMDLINE = "root=${QCOM_BOOTIMG_ROOTFS} rw rootwait console=${KERNEL_CONSOLE}"
UKI_CMDLINE += "${@d.getVar('KERNEL_CMDLINE_EXTRA') or ''}"

# Remove 'upstream' dtb, rely on EFI provided one
KERNEL_DEVICETREE = ""
KERNEL_DEVICETREE:glymur-crd = "${QCOM_DTB_DEFAULT}.dtb"
KERNEL_DEVICETREE:kaanapali-mtp = "${QCOM_DTB_DEFAULT}.dtb"
KERNEL_DEVICETREE:sm8750-mtp = "${QCOM_DTB_DEFAULT}.dtb"

# The Radxa firmware supplies a DTB too, but these images intentionally carry
# the dedicated board DTB that was built with the kernel. A standalone copy is
# also placed in the ESP.
UKI_DEVICETREE:radxa-dragon-q6a = "qcom/qcs6490-radxa-dragon-q6a.dtb"
UKI_ESP_DEVICETREE:radxa-dragon-q6a = "${UKI_DEVICETREE}"
UKI_DEVICETREE:radxa-dragon-q8b = "qcom/sc8280xp-radxa-dragon-q8b.dtb"
UKI_ESP_DEVICETREE:radxa-dragon-q8b = "${UKI_DEVICETREE}"

setup_efi_folder() {
    # Move EFI content from packages expecting /boot to be the ESP location
    if [ -d ${IMAGE_ROOTFS}/boot/EFI ]; then
        mv ${IMAGE_ROOTFS}/boot/EFI/* ${IMAGE_ROOTFS}/EFI
    fi
    find ${IMAGE_ROOTFS} -mindepth 1 ! -path "${IMAGE_ROOTFS}/EFI*" -exec rm -rf {} +
}
IMAGE_PREPROCESS_COMMAND:append = " setup_efi_folder"

do_uki[vardeps] += "KERNEL_CMDLINE_EXTRA"

# ESP image is currently only used on EFI machines
REQUIRED_MACHINE_FEATURES = "efi"
