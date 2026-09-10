DESCRIPTION = "CDT (Configuration Data Table) Firmware for Qualcomm Kaanapali platform"

SRC_URI = " \
    https://${CDT_ARTIFACTORY}/SM8850/cdt/sm8850-mtp.zip;downloadfilename=sm8850-mtp_${PV}.zip;name=sm8850-mtp \
    "
SRC_URI[sm8850-mtp.sha256sum] = "41b15acaa06311c8c7500d7467a5d8adb9fdee05aed09d983d3dad045865b1d7"

QCOM_CDT_SUBDIR = "kaanapali"

include firmware-qcom-cdt-common.inc
