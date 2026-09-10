DESCRIPTION = "CDT (Configuration Data Table) Firmware for Qualcomm Glymur platform"

SRC_URI = " \
    https://${CDT_ARTIFACTORY}/SC8480XP/cdt/sc8480xp-crd.zip;downloadfilename=sc8480xp-crd_${PV}.zip;name=sc8480xp-crd \
    "
SRC_URI[sc8480xp-crd.sha256sum] = "d694eedb0addcc5ee588d6993661cd23996fba1d1a43afc3b196dad12534bffc"

QCOM_CDT_SUBDIR = "glymur-crd/spinor"

include firmware-qcom-cdt-common.inc
