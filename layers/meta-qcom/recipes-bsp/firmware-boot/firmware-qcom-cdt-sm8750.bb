DESCRIPTION = "CDT (Configuration Data Table) Firmware for Qualcomm SM8750 platform"

SRC_URI = " \
    https://${CDT_ARTIFACTORY}/SM8750/cdt/sm8750-mtp_wcn7881.zip;downloadfilename=sm8750-mtp_wcn7881_${PV}.zip;name=sm8750-mtp_wcn7881 \
    "
SRC_URI[sm8750-mtp_wcn7881.sha256sum] = "474c35a6f06948808be118a8b0dac61ca53fd71182c1f03b92ca30d34dbb8a58"

QCOM_CDT_SUBDIR = "sm8750"

include firmware-qcom-cdt-common.inc
