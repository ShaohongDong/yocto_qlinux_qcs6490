SUMMARY = "Qualcomm qcom-tee library"
DESCRIPTION = " \
QCOM-TEE Library provides an interface for communication to \
the Qualcomm Trusted Execution Environment (QTEE) via the \
QCOM-TEE driver registered with the Linux TEE subsystem. \
"
HOMEPAGE = "https://github.com/quic/quic-teec.git"
SECTION = "libs"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE.txt;md5=2b1366ebba1ebd9ae25ad19626bbca93"

inherit cmake

SRC_URI = "git://github.com/quic/quic-teec.git;branch=main;protocol=https"
SRCREV = "a40de2d23dc04f2fad144315848c31e70c869d3d"
PV = "0.0+git"

DEPENDS += "qcbor"
