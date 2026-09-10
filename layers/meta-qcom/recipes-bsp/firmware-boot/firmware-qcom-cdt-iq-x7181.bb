DESCRIPTION = "CDT (Configuration Data Table) Firmware for Qualcomm IQ-X7181 (Hamoa) platform"

SRC_URI = " \
    https://${CDT_ARTIFACTORY}/X1E80100/cdt/IQ-X.1.2-EVK-CDT.tar.gz;downloadfilename=cdt-iq-x7181-evk_${PV}.tar.gz;name=cdt-iq-x7181-evk \
    "
SRC_URI[cdt-iq-x7181-evk.sha256sum] = "279c47ff8f1a7f4300d296fcb7fbb3d025d903e4c16f62fbb74939804949584e"

QCOM_CDT_SUBDIR = "iq-x7181/spinor"

include firmware-qcom-cdt-common.inc
