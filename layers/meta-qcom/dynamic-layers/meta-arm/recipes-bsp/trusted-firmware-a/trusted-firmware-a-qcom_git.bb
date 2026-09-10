require recipes-bsp/trusted-firmware-a/trusted-firmware-a.inc

PV = "2.14.0-qcom+git"

SRC_TAG = "tag=qcom-next-2.14-20260507"
SRC_URI = "git://github.com/qualcomm-linux/trusted-firmware-a.git;protocol=https;name=tfa;nobranch=1;${SRC_TAG}"
SRCREV_tfa = "7336923157d8e55ec6e1111b111d6c1befdb1054"

LIC_FILES_CHKSUM += "file://docs/license.rst;md5=6ed7bace7b0bc63021c6eba7b524039e"

require trusted-firmware-a-qcom.inc
