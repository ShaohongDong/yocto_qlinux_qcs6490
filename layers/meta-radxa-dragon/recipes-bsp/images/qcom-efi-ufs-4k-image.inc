# Shared UFS image settings for QCOM boards with 4 KiB logical sectors.

require qcom-efi-sd-image.inc

WKS_FILE = "${@'qcom-ota-sd.wks.in' if d.getVar('QCOM_OTA_ENABLE') == '1' else 'qcom-efi-ufs-4k.wks.in'}"
WKS_FILE:sota = "qcom-ota-sd.wks.in"
WIC_CREATE_EXTRA_ARGS += " --sector-size 4096"
