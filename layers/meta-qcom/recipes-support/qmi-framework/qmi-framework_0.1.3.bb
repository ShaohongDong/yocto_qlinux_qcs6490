SUMMARY = "QMI Framework"
DESCRIPTION = "QMI Framework is a messaging library, \
enabling users to implement clients and servers for inter-process communication (IPC)."

HOMEPAGE = "https://github.com/quic/qmi-framework"
LICENSE = "BSD-3-Clause"
LIC_FILES_CHKSUM = "file://LICENSE;md5=65b8cd575e75211d9d4ca8603167da1c"

DEPENDS = "glib-2.0"
SRCREV = "6ea813ce9fdb1126b1aa18f5447befe9119be437"

SRC_URI = "git://github.com/quic/qmi-framework.git;protocol=https;branch=main;tag=v${PV}"

inherit autotools pkgconfig
