require recipes-security/optee/optee-os.inc

PV = "4.10.0-qcom+git"

SRC_TAG = "tag=qcom-next-4.10-20260507"
SRC_URI = "git://github.com/qualcomm-linux/optee_os.git;protocol=https;name=optee;nobranch=1;${SRC_TAG}"
SRCREV_optee = "3ea006809a3ef569db4da775600cd2a46206120b"

require optee-qcom.inc
