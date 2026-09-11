# Shared GPT/ESP/rootfs image settings for QCOM boards booted by UEFI.

# These raw disk images are written directly to SD or other block media. They
# do not contain QDL partition XML files or boot-firmware payloads.
IMAGE_FSTYPES:remove = "qcomflash"
IMAGE_FSTYPES += "wic wic.bmap"

WKS_FILE = "${@'qcom-ota-sd.wks.in' if d.getVar('QCOM_OTA_ENABLE') == '1' else 'qcom-efi-sd.wks.in'}"
WKS_FILE:sota = "qcom-ota-sd.wks.in"

# WIC only needs its built-in rawcopy and rootfs plugins for this layout.
WICVARS:remove = "BBLAYERS"

# Derive the WIC root partition label from the kernel root argument. Machines
# using this common layout should set QCOM_BOOTIMG_ROOTFS to PARTLABEL=<label>.
QCOM_EFI_ROOTFS_LABEL ?= "${@d.getVar('QCOM_BOOTIMG_ROOTFS').split('=', 1)[1] if (d.getVar('QCOM_BOOTIMG_ROOTFS') or '').startswith('PARTLABEL=') else 'rootfs'}"

# Boards may select a different EFI loader and ESP layout.
QCOM_ESP_IMAGE ?= "esp-qcom-image"
do_image_wic[depends] += "${@'${QCOM_ESP_IMAGE}:do_image_complete' if d.getVar('QCOM_OTA_ENABLE') != '1' else ''}"
do_image_wic[vardeps] += "QCOM_EFI_ROOTFS_LABEL QCOM_ESP_IMAGE"
